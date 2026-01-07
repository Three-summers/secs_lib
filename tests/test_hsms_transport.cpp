#include "secs/hsms/connection.hpp"
#include "secs/hsms/message.hpp"
#include "secs/hsms/session.hpp"
#include "secs/hsms/timer.hpp"

#include "secs/core/common.hpp"
#include "secs/core/error.hpp"

#include "test_main.hpp"

#include <asio/as_tuple.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <memory>
#include <set>
#include <system_error>
#include <vector>

namespace {

using secs::core::byte;
using secs::core::bytes_view;
using secs::core::errc;
using secs::core::make_error_code;

using secs::hsms::Connection;
using secs::hsms::ConnectionOptions;
using secs::hsms::Message;
using secs::hsms::Session;
using secs::hsms::SessionOptions;
using secs::hsms::Stream;
using secs::hsms::Timer;

using namespace std::chrono_literals;

struct MemoryChannel final {
    std::deque<byte> buf{};
    bool closed{false};
    secs::core::Event data_event{};
};

class MemoryStream final : public Stream {
public:
    MemoryStream(asio::any_io_executor ex,
                 std::shared_ptr<MemoryChannel> inbox,
                 std::shared_ptr<MemoryChannel> outbox,
                 secs::core::duration write_delay = {})
        : ex_(ex), inbox_(std::move(inbox)), outbox_(std::move(outbox)),
          write_delay_(write_delay) {}

    [[nodiscard]] asio::any_io_executor executor() const noexcept override {
        return ex_;
    }
    [[nodiscard]] bool is_open() const noexcept override { return open_; }

    void cancel() noexcept override {
        if (inbox_) {
            inbox_->data_event.cancel();
        }
    }

    void close() noexcept override {
        if (!open_) {
            return;
        }
        open_ = false;
        if (outbox_) {
            outbox_->closed = true;
            outbox_->data_event.set();
        }
        if (inbox_) {
            inbox_->data_event.cancel();
        }
    }

    asio::awaitable<std::pair<std::error_code, std::size_t>>
    async_read_some(secs::core::mutable_bytes_view dst) override {
        if (!inbox_) {
            co_return std::pair{make_error_code(errc::invalid_argument),
                                std::size_t{0}};
        }

        while (inbox_->buf.empty()) {
            if (inbox_->closed) {
                co_return std::pair{
                    std::make_error_code(std::errc::broken_pipe),
                    std::size_t{0}};
            }
            auto ec = co_await inbox_->data_event.async_wait(std::nullopt);
            if (ec) {
                co_return std::pair{ec, std::size_t{0}};
            }
        }

        const std::size_t n = std::min(dst.size(), inbox_->buf.size());
        for (std::size_t i = 0; i < n; ++i) {
            dst[i] = inbox_->buf.front();
            inbox_->buf.pop_front();
        }
        if (inbox_->buf.empty()) {
            inbox_->data_event.reset();
        }

        co_return std::pair{std::error_code{}, n};
    }

    asio::awaitable<std::error_code> async_write_all(bytes_view src) override {
        if (!open_) {
            co_return make_error_code(errc::cancelled);
        }
        if (!outbox_) {
            co_return make_error_code(errc::invalid_argument);
        }
        if (write_delay_ != secs::core::duration{}) {
            asio::steady_timer t(ex_);
            t.expires_after(write_delay_);
            (void)co_await t.async_wait(asio::as_tuple(asio::use_awaitable));
        }
        outbox_->buf.insert(outbox_->buf.end(), src.begin(), src.end());
        outbox_->data_event.set();
        co_return std::error_code{};
    }

    asio::awaitable<std::error_code>
    async_connect(const asio::ip::tcp::endpoint &) override {
        co_return make_error_code(errc::invalid_argument);
    }

private:
    asio::any_io_executor ex_;
    std::shared_ptr<MemoryChannel> inbox_;
    std::shared_ptr<MemoryChannel> outbox_;
    secs::core::duration write_delay_{};
    bool open_{true};
};

struct MemoryDuplex final {
    std::unique_ptr<Stream> client_stream;
    std::unique_ptr<Stream> server_stream;
};

MemoryDuplex make_memory_duplex(asio::any_io_executor ex) {
    auto c2s = std::make_shared<MemoryChannel>();
    auto s2c = std::make_shared<MemoryChannel>();

    MemoryDuplex duplex;
    duplex.client_stream = std::make_unique<MemoryStream>(ex, s2c, c2s);
    duplex.server_stream = std::make_unique<MemoryStream>(ex, c2s, s2c);
    return duplex;
}

MemoryDuplex make_memory_duplex_with_write_delay(asio::any_io_executor ex,
                                                 secs::core::duration delay) {
    auto c2s = std::make_shared<MemoryChannel>();
    auto s2c = std::make_shared<MemoryChannel>();

    MemoryDuplex duplex;
    duplex.client_stream = std::make_unique<MemoryStream>(ex, s2c, c2s, delay);
    duplex.server_stream = std::make_unique<MemoryStream>(ex, c2s, s2c, delay);
    return duplex;
}

void test_message_encode_decode_roundtrip() {
    const std::vector<byte> body = {0xAA, 0xBB, 0xCC};
    const auto msg = secs::hsms::make_data_message(
        0x1234, 1, 2, true, 0x01020304, bytes_view{body.data(), body.size()});

    const auto frame = secs::hsms::encode_frame(msg);

    Message out;
    std::size_t consumed = 0;
    auto ec = secs::hsms::decode_frame(
        bytes_view{frame.data(), frame.size()}, out, consumed);
    TEST_EXPECT_OK(ec);
    TEST_EXPECT_EQ(consumed, frame.size());

    TEST_EXPECT_EQ(out.header.session_id, msg.header.session_id);
    TEST_EXPECT_EQ(out.header.header_byte2, msg.header.header_byte2);
    TEST_EXPECT_EQ(out.header.header_byte3, msg.header.header_byte3);
    TEST_EXPECT_EQ(out.header.p_type, msg.header.p_type);
    TEST_EXPECT_EQ(out.header.s_type, msg.header.s_type);
    TEST_EXPECT_EQ(out.header.system_bytes, msg.header.system_bytes);
    TEST_EXPECT_EQ(out.body.size(), msg.body.size());
    TEST_EXPECT(std::equal(out.body.begin(), out.body.end(), msg.body.begin()));

    // 非法：负载长度 < 头部(10B)
    std::vector<byte> bad(4 + 9, 0);
    bad[3] = 9;
    consumed = 0;
    ec = secs::hsms::decode_frame(
        bytes_view{bad.data(), bad.size()}, out, consumed);
    TEST_EXPECT_EQ(ec, make_error_code(errc::invalid_argument));

    // 未知 SType：允许解析并保留原始值，供上层发送 Reject.req。
    std::vector<byte> unknown(4 + 10, 0);
    unknown[3] = 10;
    unknown[4 + 4] = 0;    // PType（协议类型）
    unknown[4 + 5] = 0xFF; // SType（消息类型）
    ec = secs::hsms::decode_frame(
        bytes_view{unknown.data(), unknown.size()}, out, consumed);
    TEST_EXPECT_OK(ec);
    TEST_EXPECT_EQ(consumed, unknown.size());
    TEST_EXPECT_EQ(static_cast<byte>(out.header.s_type), 0xFF);

    // decode_frame：帧长度 < 4B
    std::vector<byte> too_short = {0x00, 0x01, 0x02};
    consumed = 0;
    ec = secs::hsms::decode_frame(
        bytes_view{too_short.data(), too_short.size()}, out, consumed);
    TEST_EXPECT_EQ(ec, make_error_code(errc::invalid_argument));

    // decode_frame：length 指向的帧不完整
    std::vector<byte> incomplete(4 + 10, 0);
    incomplete[0] = 0;
    incomplete[1] = 0;
    incomplete[2] = 0;
    incomplete[3] = 12; // 需要 4+12，但实际只有 4+10
    consumed = 0;
    ec = secs::hsms::decode_frame(
        bytes_view{incomplete.data(), incomplete.size()}, out, consumed);
    TEST_EXPECT_EQ(ec, make_error_code(errc::invalid_argument));

    // decode_payload：负载长度 < 头部
    std::vector<byte> payload_short(9, 0);
    ec = secs::hsms::decode_payload(
        bytes_view{payload_short.data(), payload_short.size()}, out);
    TEST_EXPECT_EQ(ec, make_error_code(errc::invalid_argument));

    // decode_payload：PType 非 0
    std::vector<byte> bad_ptype(10, 0);
    bad_ptype[4] = 0x01;
    bad_ptype[5] = static_cast<byte>(secs::hsms::SType::data);
    ec = secs::hsms::decode_payload(
        bytes_view{bad_ptype.data(), bad_ptype.size()}, out);
    TEST_EXPECT_EQ(ec, make_error_code(errc::invalid_argument));

    // decode_frame：payload_len > 16MB
    // 上限（应立即拒绝，避免后续解析与等待更多数据）
    {
        const std::uint32_t too_big = secs::hsms::kMaxPayloadSize + 1u;
        const std::array<byte, 4> len = {
            static_cast<byte>((too_big >> 24u) & 0xFFu),
            static_cast<byte>((too_big >> 16u) & 0xFFu),
            static_cast<byte>((too_big >> 8u) & 0xFFu),
            static_cast<byte>(too_big & 0xFFu),
        };
        consumed = 123; // 确认函数会重置 consumed
        ec = secs::hsms::decode_frame(
            bytes_view{len.data(), len.size()}, out, consumed);
        TEST_EXPECT_EQ(ec, make_error_code(errc::buffer_overflow));
        TEST_EXPECT_EQ(consumed, 0u);
    }

    // decode_payload：负载长度 > 16MB 上限（应一致返回 buffer_overflow）
    {
        std::vector<byte> big(
            static_cast<std::size_t>(secs::hsms::kMaxPayloadSize) + 1u, 0);
        ec =
            secs::hsms::decode_payload(bytes_view{big.data(), big.size()}, out);
        TEST_EXPECT_EQ(ec, make_error_code(errc::buffer_overflow));
    }
}

void test_encode_frame_rejects_oversized_payload() {
    const std::size_t max_body =
        static_cast<std::size_t>(secs::hsms::kMaxPayloadSize) -
        secs::hsms::kHeaderSize;
    std::vector<byte> body(max_body + 1u, 0);

    Message msg;
    msg.header.session_id = 0x0001;
    msg.header.header_byte2 = 0x01; // stream=1, W=0
    msg.header.header_byte3 = 0x01; // function=1
    msg.header.p_type = secs::hsms::kPTypeSecs2;
    msg.header.s_type = secs::hsms::SType::data;
    msg.header.system_bytes = 0x01020304;
    msg.body = std::move(body);

    std::vector<byte> frame;
    auto ec = secs::hsms::encode_frame(msg, frame);
    TEST_EXPECT_EQ(ec, make_error_code(errc::buffer_overflow));
    TEST_EXPECT(frame.empty());

    // 便捷重载在失败时返回空 vector（如需错误码请使用 out 参数版本）。
    const auto frame2 = secs::hsms::encode_frame(msg);
    TEST_EXPECT(frame2.empty());

    // PType 不支持：应返回 invalid_argument
    msg.body.clear();
    msg.header.p_type = 0x01;
    ec = secs::hsms::encode_frame(msg, frame);
    TEST_EXPECT_EQ(ec, make_error_code(errc::invalid_argument));
}

void test_connection_write_rejects_oversized_message() {
    asio::io_context ioc;
    auto duplex = make_memory_duplex(ioc.get_executor());

    Connection client_conn(std::move(duplex.client_stream),
                           ConnectionOptions{.t8 = 50ms});
    Connection server_conn(std::move(duplex.server_stream),
                           ConnectionOptions{.t8 = 50ms});
    (void)server_conn.executor();

    const std::size_t max_body =
        static_cast<std::size_t>(secs::hsms::kMaxPayloadSize) -
        secs::hsms::kHeaderSize;
    std::vector<byte> body(max_body + 1u, 0);

    Message msg;
    msg.header.session_id = 0x0001;
    msg.header.header_byte2 = 0x01;
    msg.header.header_byte3 = 0x01;
    msg.header.p_type = secs::hsms::kPTypeSecs2;
    msg.header.s_type = secs::hsms::SType::data;
    msg.header.system_bytes = 0x11223344;
    msg.body = std::move(body);

    std::atomic<bool> done{false};
    asio::co_spawn(
        ioc,
        [&client_conn, &done, msg = std::move(msg)]() -> asio::awaitable<void> {
            auto ec = co_await client_conn.async_write_message(msg);
            TEST_EXPECT_EQ(ec, make_error_code(errc::buffer_overflow));
            done = true;
            co_return;
        },
        asio::detached);

    ioc.run();
    TEST_EXPECT(done.load());
}

void test_connection_queue_limit_prioritizes_control() {
    asio::io_context ioc;
    // 用 write_delay 制造“writer 正在写第 1 条消息”的窗口，确保第 2 条 data 仍在队列里，
    // 从而覆盖 max_queue_size 与“控制消息优先”的分支。
    auto duplex = make_memory_duplex_with_write_delay(ioc.get_executor(), 50ms);

    ConnectionOptions opt{};
    opt.t8 = 0ms;
    opt.max_queue_size = 1;

    Connection conn(std::move(duplex.client_stream), opt);

    std::vector<byte> body(16, static_cast<byte>(0x11));
    const auto data1 = secs::hsms::make_data_message(
        0x0001, 1, 1, false, 0x11111111, bytes_view{body.data(), body.size()});
    const auto data2 = secs::hsms::make_data_message(
        0x0001, 1, 1, false, 0x22222222, bytes_view{body.data(), body.size()});
    const auto control = secs::hsms::make_select_req(0xFFFF, 0x33333333);

    secs::core::Event data1_done{};
    secs::core::Event data2_done{};
    std::error_code data1_ec{};
    std::error_code data2_ec{};

    std::atomic<bool> done{false};
    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            // 第 1 条 data：会被 writer_loop_ 取走并开始写（write_delay=50ms）。
            asio::co_spawn(
                ioc,
                [&]() -> asio::awaitable<void> {
                    data1_ec = co_await conn.async_write_message(data1);
                    data1_done.set();
                    co_return;
                },
                asio::detached);

            // 让第 1 条写入先启动。
            asio::steady_timer t1(ioc);
            t1.expires_after(1ms);
            (void)co_await t1.async_wait(asio::as_tuple(asio::use_awaitable));

            // 第 2 条 data：应留在队列中，等待 writer_loop_。
            asio::co_spawn(
                ioc,
                [&]() -> asio::awaitable<void> {
                    data2_ec = co_await conn.async_write_message(data2);
                    data2_done.set();
                    co_return;
                },
                asio::detached);

            // 让第 2 条写入完成入队。
            asio::steady_timer t2(ioc);
            t2.expires_after(1ms);
            (void)co_await t2.async_wait(asio::as_tuple(asio::use_awaitable));

            // 队列满时发送控制消息：应清理 data 队列为控制消息让路。
            TEST_EXPECT_OK(co_await conn.async_write_message(control));

            TEST_EXPECT_OK(co_await data2_done.async_wait(200ms));
            TEST_EXPECT_EQ(data2_ec, make_error_code(errc::buffer_overflow));

            TEST_EXPECT_OK(co_await data1_done.async_wait(1s));
            TEST_EXPECT_OK(data1_ec);

            done = true;
            ioc.stop();
            co_return;
        },
        asio::detached);

    ioc.run();
    TEST_EXPECT(done.load());
}

void test_timer_wait_and_cancel() {
    asio::io_context ioc;
    Timer t(ioc.get_executor());

    std::atomic<bool> ok1{false};
    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            auto ec = co_await t.async_wait_for(5ms);
            TEST_EXPECT_OK(ec);
            ok1 = true;
            co_return;
        },
        asio::detached);

    ioc.run();
    TEST_EXPECT(ok1.load());

    asio::io_context ioc2;
    Timer t2(ioc2.get_executor());
    asio::steady_timer cancel_timer(ioc2);
    cancel_timer.expires_after(1ms);
    cancel_timer.async_wait([&](const std::error_code &) { t2.cancel(); });

    std::atomic<bool> ok2{false};
    asio::co_spawn(
        ioc2,
        [&]() -> asio::awaitable<void> {
            auto ec = co_await t2.async_wait_for(1s);
            TEST_EXPECT_EQ(ec, make_error_code(errc::cancelled));
            ok2 = true;
            co_return;
        },
        asio::detached);

    ioc2.run();
    TEST_EXPECT(ok2.load());
}

void test_connection_loopback_framing() {
    asio::io_context ioc;
    auto duplex = make_memory_duplex(ioc.get_executor());

    Connection client_conn(std::move(duplex.client_stream),
                           ConnectionOptions{.t8 = 50ms});
    Connection server_conn(std::move(duplex.server_stream),
                           ConnectionOptions{.t8 = 50ms});
    (void)client_conn.executor();
    (void)server_conn.executor();
    TEST_EXPECT(client_conn.is_open());
    TEST_EXPECT(server_conn.is_open());

    std::atomic<bool> done{false};

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            auto [rec, msg] = co_await server_conn.async_read_message();
            TEST_EXPECT_OK(rec);
            rec = co_await server_conn.async_write_message(msg);
            TEST_EXPECT_OK(rec);
            co_return;
        },
        asio::detached);

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            const std::vector<byte> body = {0x01, 0x02, 0x03};
            const auto msg = secs::hsms::make_data_message(
                0x0001,
                1,
                1,
                false,
                0x11223344,
                bytes_view{body.data(), body.size()});

            auto ec = co_await client_conn.async_write_message(msg);
            TEST_EXPECT_OK(ec);

            auto [rec, echo] = co_await client_conn.async_read_message();
            TEST_EXPECT_OK(rec);
            TEST_EXPECT_EQ(echo.header.system_bytes, msg.header.system_bytes);
            TEST_EXPECT_EQ(echo.body.size(), msg.body.size());
            TEST_EXPECT(std::equal(
                echo.body.begin(), echo.body.end(), msg.body.begin()));

            client_conn.cancel_and_close();
            server_conn.cancel_and_close();
            TEST_EXPECT(!client_conn.is_open());
            TEST_EXPECT(!server_conn.is_open());
            done = true;
            co_return;
        },
        asio::detached);

    ioc.run();
    TEST_EXPECT(done.load());
}

void test_connection_t8_intercharacter_timeout() {
    asio::io_context ioc;
    auto duplex = make_memory_duplex(ioc.get_executor());

    auto *server_stream_ptr = duplex.server_stream.get();
    Connection client_conn(std::move(duplex.client_stream),
                           ConnectionOptions{.t8 = 10ms});

    std::atomic<bool> done{false};

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            const std::vector<byte> body = {0x10, 0x20};
            const auto msg = secs::hsms::make_data_message(
                0x0001,
                1,
                1,
                false,
                0x01020304,
                bytes_view{body.data(), body.size()});
            const auto frame = secs::hsms::encode_frame(msg);

            const std::size_t first = 4 + 3;
            auto ec = co_await server_stream_ptr->async_write_all(
                bytes_view{frame.data(), first});
            TEST_EXPECT_OK(ec);

            asio::steady_timer delay(ioc);
            delay.expires_after(50ms);
            (void)co_await delay.async_wait(
                asio::as_tuple(asio::use_awaitable));

            (void)co_await server_stream_ptr->async_write_all(
                bytes_view{frame.data() + first, frame.size() - first});
            co_return;
        },
        asio::detached);

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            auto [rec, _] = co_await client_conn.async_read_message();
            TEST_EXPECT_EQ(rec, make_error_code(errc::timeout));
            client_conn.cancel_and_close();
            done = true;
            co_return;
        },
        asio::detached);

    ioc.run();
    TEST_EXPECT(done.load());
}

void test_connection_t8_disabled() {
    asio::io_context ioc;
    auto duplex = make_memory_duplex(ioc.get_executor());

    Connection client_conn(std::move(duplex.client_stream),
                           ConnectionOptions{.t8 = secs::core::duration{}});
    Connection server_conn(std::move(duplex.server_stream),
                           ConnectionOptions{.t8 = secs::core::duration{}});

    std::atomic<bool> done{false};

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            auto [rec, msg] = co_await server_conn.async_read_message();
            TEST_EXPECT_OK(rec);
            rec = co_await server_conn.async_write_message(msg);
            TEST_EXPECT_OK(rec);
            co_return;
        },
        asio::detached);

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            const std::vector<byte> body = {0x01};
            const auto msg = secs::hsms::make_data_message(
                0x0001,
                1,
                1,
                false,
                0xAABBCCDD,
                bytes_view{body.data(), body.size()});

            auto ec = co_await client_conn.async_write_message(msg);
            TEST_EXPECT_OK(ec);

            auto [rec, echo] = co_await client_conn.async_read_message();
            TEST_EXPECT_OK(rec);
            TEST_EXPECT_EQ(echo.header.system_bytes, msg.header.system_bytes);

            client_conn.cancel_and_close();
            server_conn.cancel_and_close();
            done = true;
            co_return;
        },
        asio::detached);

    ioc.run();
    TEST_EXPECT(done.load());
}

void test_connection_null_stream_and_tcpstream_error_paths() {
    asio::io_context ioc;

    Connection null_conn(std::unique_ptr<Stream>{}, ConnectionOptions{});
    (void)null_conn.executor();
    TEST_EXPECT(!null_conn.is_open());
    null_conn.cancel_and_close();

    Connection tcp_conn(ioc.get_executor(),
                        ConnectionOptions{.t8 = secs::core::duration{}});
    (void)tcp_conn.executor();
    TEST_EXPECT(!tcp_conn.is_open());

    std::atomic<bool> done{false};
    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            // 空 Stream：所有异步操作应返回 invalid_argument
            const asio::ip::tcp::endpoint ep{
                asio::ip::make_address("127.0.0.1"), 1};
            auto ec = co_await null_conn.async_connect(ep);
            TEST_EXPECT_EQ(ec, make_error_code(errc::invalid_argument));

            ec = co_await null_conn.async_write_message(
                secs::hsms::make_select_req(0x0001, 0x1));
            TEST_EXPECT_EQ(ec, make_error_code(errc::invalid_argument));

            auto [rec, _] = co_await null_conn.async_read_message();
            TEST_EXPECT_EQ(rec, make_error_code(errc::invalid_argument));

            // TCP Stream：未连接时读写应返回系统错误（不抛异常）
            ec = co_await tcp_conn.async_write_message(
                secs::hsms::make_select_req(0x0001, 0x2));
            TEST_EXPECT(ec.value() != 0);

            std::tie(rec, std::ignore) = co_await tcp_conn.async_read_message();
            TEST_EXPECT(rec.value() != 0);

            tcp_conn.cancel_and_close();
            done = true;
            co_return;
        },
        asio::detached);

    ioc.run();
    TEST_EXPECT(done.load());
}

void test_connection_async_read_message_invalid_frames() {
    asio::io_context ioc;
    auto duplex = make_memory_duplex(ioc.get_executor());

    auto *server_stream_ptr = duplex.server_stream.get();
    Connection client_conn(std::move(duplex.client_stream),
                           ConnectionOptions{.t8 = secs::core::duration{}});

    std::atomic<bool> done{false};

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            // 1) 负载长度 payload_len < header_len
            std::array<byte, 4> len9 = {0x00, 0x00, 0x00, 0x09};
            auto ec = co_await server_stream_ptr->async_write_all(
                bytes_view{len9.data(), len9.size()});
            TEST_EXPECT_OK(ec);

            auto [rec, _] = co_await client_conn.async_read_message();
            TEST_EXPECT_EQ(rec, make_error_code(errc::invalid_argument));

            // 2) decode_payload 失败：PType 非 0
            std::vector<byte> frame(4 + 10, 0);
            frame[3] = 10;
            frame[4 + 4] = 0x01;
            frame[4 + 5] = static_cast<byte>(secs::hsms::SType::data);
            ec = co_await server_stream_ptr->async_write_all(
                bytes_view{frame.data(), frame.size()});
            TEST_EXPECT_OK(ec);

            std::tie(rec, std::ignore) =
                co_await client_conn.async_read_message();
            TEST_EXPECT_EQ(rec, make_error_code(errc::invalid_argument));

            // 3) payload_len > 16MB 上限
            {
                const std::uint32_t too_big = secs::hsms::kMaxPayloadSize + 1u;
                const std::array<byte, 4> len = {
                    static_cast<byte>((too_big >> 24u) & 0xFFu),
                    static_cast<byte>((too_big >> 16u) & 0xFFu),
                    static_cast<byte>((too_big >> 8u) & 0xFFu),
                    static_cast<byte>(too_big & 0xFFu),
                };
                ec = co_await server_stream_ptr->async_write_all(
                    bytes_view{len.data(), len.size()});
                TEST_EXPECT_OK(ec);

                std::tie(rec, std::ignore) =
                    co_await client_conn.async_read_message();
                TEST_EXPECT_EQ(rec, make_error_code(errc::buffer_overflow));
            }

            // 4) decode_payload：未知 SType 允许解析（供上层发送 Reject）
            {
                std::vector<byte> frame(4 + 10, 0);
                frame[3] = 10;
                frame[4 + 4] = secs::hsms::kPTypeSecs2;
                frame[4 + 5] = 0xFF;
                ec = co_await server_stream_ptr->async_write_all(
                    bytes_view{frame.data(), frame.size()});
                TEST_EXPECT_OK(ec);

                Message got;
                std::tie(rec, got) = co_await client_conn.async_read_message();
                TEST_EXPECT_OK(rec);
                TEST_EXPECT_EQ(static_cast<byte>(got.header.s_type), 0xFF);
            }

            client_conn.cancel_and_close();
            done = true;
            co_return;
        },
        asio::detached);

    ioc.run();
    TEST_EXPECT(done.load());
}

void test_connection_write_serialization_waiters() {
    asio::io_context ioc;

    // 单向即可：验证 async_write_message 的“写锁等待”分支能被触发且不会死锁。
    auto c2s = std::make_shared<MemoryChannel>();
    auto s2c = std::make_shared<MemoryChannel>();

    auto slow_client_stream =
        std::make_unique<MemoryStream>(ioc.get_executor(), s2c, c2s, 10ms);
    Connection client_conn(std::move(slow_client_stream),
                           ConnectionOptions{.t8 = secs::core::duration{}});

    std::atomic<int> finished{0};
    std::atomic<bool> done{false};

    auto send_one = [&](std::uint32_t sb) -> asio::awaitable<void> {
        auto ec = co_await client_conn.async_write_message(
            secs::hsms::make_select_req(0x0001, sb));
        TEST_EXPECT_OK(ec);
        ++finished;
        co_return;
    };

    asio::co_spawn(ioc, send_one(0x10), asio::detached);
    asio::co_spawn(ioc, send_one(0x11), asio::detached);

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            while (finished.load() != 2) {
                asio::steady_timer t(ioc);
                t.expires_after(1ms);
                (void)co_await t.async_wait(
                    asio::as_tuple(asio::use_awaitable));
            }
            client_conn.cancel_and_close();
            done = true;
            co_return;
        },
        asio::detached);

    ioc.run();
    TEST_EXPECT(done.load());
}

void test_session_select_and_linktest() {
    asio::io_context ioc;

    SessionOptions opt;
    opt.session_id = 0x0001;
    opt.t6 = 50ms;
    opt.t7 = 200ms;
    opt.t8 = 50ms;

    Session server(ioc.get_executor(), opt);
    Session client(ioc.get_executor(), opt);

    auto duplex = make_memory_duplex(ioc.get_executor());
    Connection client_conn(std::move(duplex.client_stream),
                           ConnectionOptions{.t8 = opt.t8});
    Connection server_conn(std::move(duplex.server_stream),
                           ConnectionOptions{.t8 = opt.t8});

    std::atomic<bool> done{false};

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            auto ec =
                co_await server.async_open_passive(std::move(server_conn));
            TEST_EXPECT_OK(ec);
            co_return;
        },
        asio::detached);

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            auto ec = co_await client.async_open_active(std::move(client_conn));
            TEST_EXPECT_OK(ec);

            ec = co_await client.async_linktest();
            TEST_EXPECT_OK(ec);

            client.stop();
            server.stop();
            done = true;
            co_return;
        },
        asio::detached);

    ioc.run();
    TEST_EXPECT(done.load());
}

void test_session_select_req_when_already_selected() {
    asio::io_context ioc;

    SessionOptions opt;
    opt.session_id = 0x0001;
    opt.t6 = 50ms;
    opt.t7 = 200ms;
    opt.t8 = 50ms;

    Session server(ioc.get_executor(), opt);
    Session client(ioc.get_executor(), opt);

    auto duplex = make_memory_duplex(ioc.get_executor());
    Connection client_conn(std::move(duplex.client_stream),
                           ConnectionOptions{.t8 = opt.t8});
    Connection server_conn(std::move(duplex.server_stream),
                           ConnectionOptions{.t8 = opt.t8});

    std::atomic<bool> done{false};

    asio::co_spawn(
        ioc, server.async_open_passive(std::move(server_conn)), asio::detached);

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            auto ec = co_await client.async_open_active(std::move(client_conn));
            TEST_EXPECT_OK(ec);

            // 已经进入“已选择”状态后再次发送 SELECT.req，触发服务端“already
            // selected”分支。
            ec = co_await client.async_send(secs::hsms::make_select_req(
                0xFFFF, client.allocate_system_bytes()));
            TEST_EXPECT_OK(ec);

            // 稍等让控制消息往返处理完成。
            asio::steady_timer t(ioc);
            t.expires_after(5ms);
            (void)co_await t.async_wait(asio::as_tuple(asio::use_awaitable));

            TEST_EXPECT(server.is_selected());
            TEST_EXPECT(client.is_selected());

            client.stop();
            server.stop();
            done = true;
            co_return;
        },
        asio::detached);

    ioc.run();
    TEST_EXPECT(done.load());
}

void test_session_select_req_session_id_mismatch_disconnects() {
    asio::io_context ioc;

    SessionOptions opt;
    opt.session_id = 0x0001;
    opt.t6 = 50ms;
    opt.t7 = 200ms;
    opt.t8 = 50ms;

    Session server(ioc.get_executor(), opt);
    Session client(ioc.get_executor(), opt);

    auto duplex = make_memory_duplex(ioc.get_executor());
    Connection client_conn(std::move(duplex.client_stream),
                           ConnectionOptions{.t8 = opt.t8});
    Connection server_conn(std::move(duplex.server_stream),
                           ConnectionOptions{.t8 = opt.t8});

    std::atomic<bool> done{false};

    asio::co_spawn(
        ioc, server.async_open_passive(std::move(server_conn)), asio::detached);

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            auto ec = co_await client.async_open_active(std::move(client_conn));
            TEST_EXPECT_OK(ec);
            TEST_EXPECT(client.is_selected());
            TEST_EXPECT(server.is_selected());

            // HSMS-SS：控制消息 SessionID 必须为 0xFFFF。发送非法 SessionID 的控制消息，
            // 期望双方断线收敛。
            ec = co_await client.async_send(secs::hsms::make_select_req(
                /*session_id=*/0x0001, client.allocate_system_bytes()));
            TEST_EXPECT_OK(ec);

            TEST_EXPECT_OK(co_await client.async_wait_reader_stopped(200ms));
            TEST_EXPECT_OK(co_await server.async_wait_reader_stopped(200ms));

            TEST_EXPECT_EQ(server.state(),
                           secs::hsms::SessionState::disconnected);
            TEST_EXPECT_EQ(client.state(),
                           secs::hsms::SessionState::disconnected);

            client.stop();
            server.stop();
            done = true;
            co_return;
        },
        asio::detached);

    ioc.run();
    TEST_EXPECT(done.load());
}

void test_session_deselect_req_moves_to_not_selected() {
    asio::io_context ioc;

    SessionOptions opt;
    opt.session_id = 0x0001;
    opt.t6 = 50ms;
    opt.t7 = 200ms;
    opt.t8 = 50ms;

    Session server(ioc.get_executor(), opt);
    Session client(ioc.get_executor(), opt);

    auto duplex = make_memory_duplex(ioc.get_executor());
    Connection client_conn(std::move(duplex.client_stream),
                           ConnectionOptions{.t8 = opt.t8});
    Connection server_conn(std::move(duplex.server_stream),
                           ConnectionOptions{.t8 = opt.t8});

    std::atomic<bool> done{false};

    asio::co_spawn(
        ioc, server.async_open_passive(std::move(server_conn)), asio::detached);

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            auto ec = co_await client.async_open_active(std::move(client_conn));
            TEST_EXPECT_OK(ec);

            ec = co_await client.async_send(secs::hsms::make_deselect_req(
                0xFFFF, client.allocate_system_bytes()));
            TEST_EXPECT_OK(ec);

            asio::steady_timer t(ioc);
            t.expires_after(10ms);
            (void)co_await t.async_wait(asio::as_tuple(asio::use_awaitable));

            TEST_EXPECT_EQ(server.state(), secs::hsms::SessionState::connected);
            TEST_EXPECT_EQ(client.state(), secs::hsms::SessionState::connected);
            TEST_EXPECT(!server.is_selected());
            TEST_EXPECT(!client.is_selected());

            // NOT_SELECTED 期间不允许发送 data message
            Message data = secs::hsms::make_data_message(
                opt.session_id,
                1,
                1,
                false,
                client.allocate_system_bytes(),
                bytes_view{});
            auto send_ec = co_await client.async_send(data);
            TEST_EXPECT_EQ(send_ec, make_error_code(errc::invalid_argument));

            client.stop();
            server.stop();
            done = true;
            co_return;
        },
        asio::detached);

    ioc.run();
    TEST_EXPECT(done.load());
}

void test_session_select_reject() {
    asio::io_context ioc;

    SessionOptions server_opt;
    server_opt.session_id = 0x0001;
    server_opt.passive_accept_select = false;
    server_opt.t6 = 50ms;
    server_opt.t7 = 200ms;
    server_opt.t8 = 50ms;

    SessionOptions client_opt = server_opt;
    client_opt.passive_accept_select = true;

    Session server(ioc.get_executor(), server_opt);
    Session client(ioc.get_executor(), client_opt);

    auto duplex = make_memory_duplex(ioc.get_executor());
    Connection client_conn(std::move(duplex.client_stream),
                           ConnectionOptions{.t8 = client_opt.t8});
    Connection server_conn(std::move(duplex.server_stream),
                           ConnectionOptions{.t8 = server_opt.t8});

    std::atomic<bool> done{false};

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            (void)co_await server.async_open_passive(std::move(server_conn));
            co_return;
        },
        asio::detached);

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            auto ec = co_await client.async_open_active(std::move(client_conn));
            TEST_EXPECT_EQ(ec, make_error_code(errc::invalid_argument));
            client.stop();
            server.stop();
            done = true;
            co_return;
        },
        asio::detached);

    ioc.run();
    TEST_EXPECT(done.load());
}

void test_session_open_passive_socket_overload_executes() {
    asio::io_context ioc;

    SessionOptions opt;
    opt.session_id = 0x0001;
    opt.t6 = 5ms;
    opt.t7 = 5ms;
    opt.t8 = secs::core::duration{};

    Session server(ioc.get_executor(), opt);

    std::atomic<bool> done{false};
    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            asio::ip::tcp::socket sock(ioc.get_executor());
            auto ec = co_await server.async_open_passive(std::move(sock));
            TEST_EXPECT(ec.value() != 0);
            server.stop();
            done = true;
            co_return;
        },
        asio::detached);

    ioc.run();
    TEST_EXPECT(done.load());
}

void test_session_precondition_and_stop_branches() {
    asio::io_context ioc;

    SessionOptions opt;
    opt.session_id = 0x0001;
    opt.t3 = 5ms;
    opt.t6 = 5ms;
    opt.t7 = 5ms;
    opt.t8 = secs::core::duration{};

    Session client(ioc.get_executor(), opt);

    auto duplex = make_memory_duplex(ioc.get_executor());
    Connection conn(std::move(duplex.client_stream),
                    ConnectionOptions{.t8 = secs::core::duration{}});
    Connection peer(std::move(duplex.server_stream),
                    ConnectionOptions{.t8 = secs::core::duration{}});
    (void)peer;

    std::atomic<bool> done{false};
    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            // 未连接：async_send/async_linktest/async_request_data 应返回
            // invalid_argument
            const std::vector<byte> body = {0x01};
            auto ec = co_await client.async_send(secs::hsms::make_data_message(
                opt.session_id,
                1,
                1,
                false,
                0x01,
                bytes_view{body.data(), body.size()}));
            TEST_EXPECT_EQ(ec, make_error_code(errc::invalid_argument));

            ec = co_await client.async_linktest();
            TEST_EXPECT_EQ(ec, make_error_code(errc::invalid_argument));

            auto [rec, _] = co_await client.async_request_data(
                1, 1, bytes_view{body.data(), body.size()});
            TEST_EXPECT_EQ(rec, make_error_code(errc::invalid_argument));

            std::tie(rec, std::ignore) =
                co_await client.async_receive_data(5ms);
            TEST_EXPECT_EQ(rec, make_error_code(errc::timeout));

            // stop 后 open_* 应返回 errc::cancelled
            client.stop();
            ec = co_await client.async_open_active(std::move(conn));
            TEST_EXPECT_EQ(ec, make_error_code(errc::cancelled));
            done = true;
            co_return;
        },
        asio::detached);

    ioc.run();
    TEST_EXPECT(done.load());
}

void test_session_unknown_control_type_is_ignored() {
    asio::io_context ioc;

    SessionOptions opt;
    opt.session_id = 0x0001;
    opt.t6 = 50ms;
    opt.t7 = 200ms;
    opt.t8 = 50ms;

    Session server(ioc.get_executor(), opt);
    Session client(ioc.get_executor(), opt);

    auto duplex = make_memory_duplex(ioc.get_executor());
    Connection client_conn(std::move(duplex.client_stream),
                           ConnectionOptions{.t8 = opt.t8});
    Connection server_conn(std::move(duplex.server_stream),
                           ConnectionOptions{.t8 = opt.t8});

    std::atomic<bool> done{false};
    asio::co_spawn(
        ioc, server.async_open_passive(std::move(server_conn)), asio::detached);

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            auto ec = co_await client.async_open_active(std::move(client_conn));
            TEST_EXPECT_OK(ec);

            // 发送 Reject.req（当前实现走“默认分支”，忽略）。
            Message m;
            m.header.session_id = 0xFFFF;
            m.header.p_type = secs::hsms::kPTypeSecs2;
            m.header.s_type = secs::hsms::SType::reject_req;
            m.header.system_bytes = client.allocate_system_bytes();
            ec = co_await client.async_send(m);
            TEST_EXPECT_OK(ec);

            asio::steady_timer t(ioc);
            t.expires_after(5ms);
            (void)co_await t.async_wait(asio::as_tuple(asio::use_awaitable));

            TEST_EXPECT(client.is_selected());
            TEST_EXPECT(server.is_selected());

            client.stop();
            server.stop();
            done = true;
            co_return;
        },
        asio::detached);

    ioc.run();
    TEST_EXPECT(done.load());
}

void test_session_control_response_type_mismatch_times_out() {
    asio::io_context ioc;

    SessionOptions opt;
    opt.session_id = 0x0001;
    opt.t6 = 20ms;
    opt.t7 = 200ms;
    opt.t8 = secs::core::duration{};

    Session client(ioc.get_executor(), opt);

    auto duplex = make_memory_duplex(ioc.get_executor());
    Connection client_conn(std::move(duplex.client_stream),
                           ConnectionOptions{.t8 = secs::core::duration{}});
    Connection server_conn(std::move(duplex.server_stream),
                           ConnectionOptions{.t8 = secs::core::duration{}});

    std::atomic<bool> done{false};

    // 服务端：只实现 select 握手；对 linktest.req 返回一个“错误类型”的
    // select.rsp，导致客户端挂起请求无法完成，最终超时。
    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            auto [ec1, m1] = co_await server_conn.async_read_message();
            TEST_EXPECT_OK(ec1);
            TEST_EXPECT_EQ(m1.header.s_type, secs::hsms::SType::select_req);
            ec1 = co_await server_conn.async_write_message(
                secs::hsms::make_select_rsp(
                    m1.header.session_id, 0, m1.header.system_bytes));
            TEST_EXPECT_OK(ec1);

            auto [ec2, m2] = co_await server_conn.async_read_message();
            TEST_EXPECT_OK(ec2);
            TEST_EXPECT_EQ(m2.header.s_type, secs::hsms::SType::linktest_req);

            // 故意使用相同 SystemBytes 回一个 select.rsp（而不是 linktest.rsp）
            (void)co_await server_conn.async_write_message(
                secs::hsms::make_select_rsp(
                    m2.header.session_id, 0, m2.header.system_bytes));
            co_return;
        },
        asio::detached);

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            auto ec = co_await client.async_open_active(std::move(client_conn));
            TEST_EXPECT_OK(ec);

            ec = co_await client.async_linktest();
            TEST_EXPECT_EQ(ec, make_error_code(errc::timeout));

            client.stop();
            server_conn.cancel_and_close();
            done = true;
            co_return;
        },
        asio::detached);

    ioc.run();
    TEST_EXPECT(done.load());
}

void test_session_t6_timeout_on_select() {
    asio::io_context ioc;

    SessionOptions client_opt;
    client_opt.session_id = 0x0001;
    client_opt.t6 = 20ms;
    client_opt.t8 = 50ms;

    Session client(ioc.get_executor(), client_opt);

    auto duplex = make_memory_duplex(ioc.get_executor());
    Connection client_conn(std::move(duplex.client_stream),
                           ConnectionOptions{.t8 = client_opt.t8});

    std::atomic<bool> done{false};

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            auto ec = co_await client.async_open_active(std::move(client_conn));
            TEST_EXPECT_EQ(ec, make_error_code(errc::timeout));
            client.stop();
            done = true;
            co_return;
        },
        asio::detached);

    ioc.run();
    TEST_EXPECT(done.load());
}

void test_session_select_timeout_late_select_rsp_is_ignored() {
    asio::io_context ioc;

    SessionOptions opt;
    opt.session_id = 0x0001;
    opt.t6 = 20ms;
    opt.t7 = 200ms;
    opt.t8 = secs::core::duration{};

    Session client(ioc.get_executor(), opt);

    auto duplex = make_memory_duplex(ioc.get_executor());
    Connection client_conn(std::move(duplex.client_stream),
                           ConnectionOptions{.t8 = opt.t8});
    Connection server_conn(std::move(duplex.server_stream),
                           ConnectionOptions{.t8 = opt.t8});

    std::atomic<bool> done{false};

    // 服务端：收到 Select.req 后延迟到 T6+δ 再回 Select.rsp(OK)。
    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            auto [ec1, m1] = co_await server_conn.async_read_message();
            TEST_EXPECT_OK(ec1);
            TEST_EXPECT_EQ(m1.header.s_type, secs::hsms::SType::select_req);

            asio::steady_timer t(ioc);
            t.expires_after(opt.t6 + 10ms);
            (void)co_await t.async_wait(asio::as_tuple(asio::use_awaitable));

            ec1 = co_await server_conn.async_write_message(
                secs::hsms::make_select_rsp(
                    m1.header.session_id, 0, m1.header.system_bytes));
            TEST_EXPECT_OK(ec1);
            co_return;
        },
        asio::detached);

    asio::co_spawn(
        ioc,
        [&, client_conn = std::move(client_conn)]() mutable
        -> asio::awaitable<void> {
            const auto gen0 = client.selected_generation();

            auto ec = co_await client.async_open_active(std::move(client_conn));
            TEST_EXPECT_EQ(ec, make_error_code(errc::timeout));

            // 迟到的 Select.rsp 不得把会话推进到 selected，也不得递增 generation。
            asio::steady_timer t(ioc);
            t.expires_after(40ms);
            (void)co_await t.async_wait(asio::as_tuple(asio::use_awaitable));

            // T6 超时按“通信失败”处理：会话应断线收敛，迟到响应不得改变状态。
            TEST_EXPECT_EQ(client.state(),
                           secs::hsms::SessionState::disconnected);
            TEST_EXPECT(!client.is_selected());
            TEST_EXPECT_EQ(client.selected_generation(), gen0);

            client.stop();
            server_conn.cancel_and_close();
            done = true;
            co_return;
        },
        asio::detached);

    ioc.run();
    TEST_EXPECT(done.load());
}

void test_session_pending_cancelled_on_disconnect() {
    asio::io_context ioc;

    SessionOptions opt;
    opt.session_id = 0x0001;
    opt.t3 = 200ms;
    opt.t6 = 20ms;
    opt.t7 = 200ms;
    opt.t8 = secs::core::duration{};

    Session client(ioc.get_executor(), opt);

    auto duplex = make_memory_duplex(ioc.get_executor());
    Connection client_conn(std::move(duplex.client_stream),
                           ConnectionOptions{.t8 = secs::core::duration{}});
    Connection server_conn(std::move(duplex.server_stream),
                           ConnectionOptions{.t8 = secs::core::duration{}});

    std::atomic<bool> done{false};

    // 服务端：select 成功后，收到 data 后立即发送 separate.req
    // 断线，触发客户端挂起请求被 on_disconnected_ 取消。
    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            auto [ec1, m1] = co_await server_conn.async_read_message();
            TEST_EXPECT_OK(ec1);
            TEST_EXPECT_EQ(m1.header.s_type, secs::hsms::SType::select_req);
            ec1 = co_await server_conn.async_write_message(
                secs::hsms::make_select_rsp(
                    m1.header.session_id, 0, m1.header.system_bytes));
            TEST_EXPECT_OK(ec1);

            auto [ec2, m2] = co_await server_conn.async_read_message();
            TEST_EXPECT_OK(ec2);
            TEST_EXPECT_EQ(m2.header.s_type, secs::hsms::SType::data);

            (void)co_await server_conn.async_write_message(
                secs::hsms::make_separate_req(0xFFFF, 0xABCDEF01));
            (void)co_await server_conn.async_close();
            co_return;
        },
        asio::detached);

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            auto ec = co_await client.async_open_active(std::move(client_conn));
            TEST_EXPECT_OK(ec);

            const std::vector<byte> body = {0x01};
            auto [rec, _] = co_await client.async_request_data(
                1, 1, bytes_view{body.data(), body.size()});
            TEST_EXPECT(rec.value() != 0);
            TEST_EXPECT(rec != make_error_code(errc::timeout));

            client.stop();
            server_conn.cancel_and_close();
            done = true;
            co_return;
        },
        asio::detached);

    ioc.run();
    TEST_EXPECT(done.load());
}

void test_session_pending_limit_returns_buffer_overflow() {
    asio::io_context ioc;

    SessionOptions opt;
    opt.session_id = 0x0001;
    opt.t3 = 1s;
    opt.t6 = 20ms;
    opt.t7 = 200ms;
    opt.t8 = secs::core::duration{};
    opt.max_pending_requests = 1;

    Session server(ioc.get_executor(), opt);
    Session client(ioc.get_executor(), opt);

    auto duplex = make_memory_duplex(ioc.get_executor());
    Connection client_conn(std::move(duplex.client_stream),
                           ConnectionOptions{.t8 = secs::core::duration{}});
    Connection server_conn(std::move(duplex.server_stream),
                           ConnectionOptions{.t8 = secs::core::duration{}});

    secs::core::Event req1_done{};
    std::error_code req1_ec{};

    std::atomic<bool> done{false};
    asio::co_spawn(
        ioc, server.async_open_passive(std::move(server_conn)), asio::detached);

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            TEST_EXPECT_OK(
                co_await client.async_open_active(std::move(client_conn)));

            const std::vector<byte> body = {static_cast<byte>(0x01)};

            // 第 1 个请求：保持挂起，不等待其自然超时（通过 stop() 取消）。
            asio::co_spawn(
                ioc,
                [&]() -> asio::awaitable<void> {
                    auto [ec, _] = co_await client.async_request_data(
                        1,
                        1,
                        bytes_view{body.data(), body.size()},
                        /*timeout=*/1s);
                    req1_ec = ec;
                    req1_done.set();
                    co_return;
                },
                asio::detached);

            // 给第 1 个请求一个调度窗口，让其先插入 pending_。
            asio::steady_timer t(ioc);
            t.expires_after(1ms);
            (void)co_await t.async_wait(asio::as_tuple(asio::use_awaitable));

            // 第 2 个并发请求：应因 pending 上限而快速失败。
            auto [ec2, _2] = co_await client.async_request_data(
                1,
                1,
                bytes_view{body.data(), body.size()},
                /*timeout=*/1s);
            TEST_EXPECT_EQ(ec2, make_error_code(errc::buffer_overflow));

            client.stop();
            server.stop();

            TEST_EXPECT_OK(co_await req1_done.async_wait(200ms));
            TEST_EXPECT_EQ(req1_ec, make_error_code(errc::cancelled));

            done = true;
            ioc.stop();
            co_return;
        },
        asio::detached);

    ioc.run();
    TEST_EXPECT(done.load());
}

void test_session_deselect_drops_inbound_data_when_not_selected() {
    asio::io_context ioc;

    SessionOptions opt;
    opt.session_id = 0x0001;
    opt.t6 = 50ms;
    opt.t7 = 200ms;
    opt.t8 = secs::core::duration{};

    Session client(ioc.get_executor(), opt);

    auto duplex = make_memory_duplex(ioc.get_executor());
    Connection client_conn(std::move(duplex.client_stream),
                           ConnectionOptions{.t8 = opt.t8});
    Connection server_conn(std::move(duplex.server_stream),
                           ConnectionOptions{.t8 = opt.t8});

    std::atomic<bool> done{false};

    // 服务端：完成 Select -> Deselect 往返后，违规发送一条 data message。
    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            auto [ec1, m1] = co_await server_conn.async_read_message();
            TEST_EXPECT_OK(ec1);
            TEST_EXPECT_EQ(m1.header.s_type, secs::hsms::SType::select_req);
            ec1 = co_await server_conn.async_write_message(
                secs::hsms::make_select_rsp(
                    m1.header.session_id, 0, m1.header.system_bytes));
            TEST_EXPECT_OK(ec1);

            auto [ec2, m2] = co_await server_conn.async_read_message();
            TEST_EXPECT_OK(ec2);
            TEST_EXPECT_EQ(m2.header.s_type, secs::hsms::SType::deselect_req);
            ec2 = co_await server_conn.async_write_message(
                secs::hsms::make_deselect_rsp(
                    m2.header.session_id, 0, m2.header.system_bytes));
            TEST_EXPECT_OK(ec2);

            const std::vector<byte> body = {0xDE, 0xAD};
            auto data = secs::hsms::make_data_message(
                opt.session_id,
                1,
                1,
                false,
                0x11111111,
                bytes_view{body.data(), body.size()});
            (void)co_await server_conn.async_write_message(data);
            co_return;
        },
        asio::detached);

    asio::co_spawn(
        ioc,
        [&, client_conn = std::move(client_conn)]() mutable
        -> asio::awaitable<void> {
            auto ec = co_await client.async_open_active(std::move(client_conn));
            TEST_EXPECT_OK(ec);
            TEST_EXPECT(client.is_selected());

            ec = co_await client.async_send(secs::hsms::make_deselect_req(
                0xFFFF, client.allocate_system_bytes()));
            TEST_EXPECT_OK(ec);

            asio::steady_timer t(ioc);
            t.expires_after(20ms);
            (void)co_await t.async_wait(asio::as_tuple(asio::use_awaitable));

            TEST_EXPECT_EQ(client.state(), secs::hsms::SessionState::connected);
            TEST_EXPECT(!client.is_selected());

            // NOT_SELECTED 期间收到的 data message 必须被丢弃，不得进入 inbound 队列。
            auto [rec, _] = co_await client.async_receive_data(20ms);
            TEST_EXPECT_EQ(rec, make_error_code(errc::timeout));

            client.stop();
            server_conn.cancel_and_close();
            done = true;
            co_return;
        },
        asio::detached);

    ioc.run();
    TEST_EXPECT(done.load());
}

void test_session_t3_reply_timeout() {
    asio::io_context ioc;

    SessionOptions opt;
    opt.session_id = 0x0001;
    opt.t3 = 20ms;
    opt.t6 = 50ms;
    opt.t7 = 200ms;
    opt.t8 = 50ms;

    Session server(ioc.get_executor(), opt);
    Session client(ioc.get_executor(), opt);

    auto duplex = make_memory_duplex(ioc.get_executor());
    Connection client_conn(std::move(duplex.client_stream),
                           ConnectionOptions{.t8 = opt.t8});
    Connection server_conn(std::move(duplex.server_stream),
                           ConnectionOptions{.t8 = opt.t8});

    std::atomic<bool> done{false};

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            auto ec =
                co_await server.async_open_passive(std::move(server_conn));
            TEST_EXPECT_OK(ec);

            // 读取一条数据消息但不回复，触发客户端 T3。
            auto [rec, _] = co_await server.async_receive_data(200ms);
            TEST_EXPECT_OK(rec);
            co_return;
        },
        asio::detached);

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            auto ec = co_await client.async_open_active(std::move(client_conn));
            TEST_EXPECT_OK(ec);

            const std::vector<byte> body = {0xDE, 0xAD};
            auto [rec, _] = co_await client.async_request_data(
                1, 1, bytes_view{body.data(), body.size()});
            TEST_EXPECT_EQ(rec, make_error_code(errc::timeout));

            client.stop();
            server.stop();
            done = true;
            co_return;
        },
        asio::detached);

    ioc.run();
    TEST_EXPECT(done.load());
}

void test_session_linktest_interval_disconnect_on_failure() {
    asio::io_context ioc;

    SessionOptions opt;
    opt.session_id = 0x0001;
    opt.t6 = 10ms;
    opt.t7 = 200ms;
    opt.t8 = secs::core::duration{};
    opt.linktest_interval = 5ms;

    Session client(ioc.get_executor(), opt);

    auto duplex = make_memory_duplex(ioc.get_executor());
    Connection client_conn(std::move(duplex.client_stream),
                           ConnectionOptions{.t8 = secs::core::duration{}});
    Connection server_conn(std::move(duplex.server_stream),
                           ConnectionOptions{.t8 = secs::core::duration{}});

    std::atomic<bool> done{false};

    // 服务端：select 成功后不回应任何 linktest.req，促使客户端 linktest_loop_
    // 触发超时并断开。
    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            auto [ec1, m1] = co_await server_conn.async_read_message();
            TEST_EXPECT_OK(ec1);
            TEST_EXPECT_EQ(m1.header.s_type, secs::hsms::SType::select_req);
            ec1 = co_await server_conn.async_write_message(
                secs::hsms::make_select_rsp(
                    m1.header.session_id, 0, m1.header.system_bytes));
            TEST_EXPECT_OK(ec1);

            // 读取并丢弃若干 linktest.req（不回复）
            for (int i = 0; i < 3; ++i) {
                auto [ec2, m2] = co_await server_conn.async_read_message();
                if (ec2) {
                    break;
                }
                (void)m2;
            }
            co_return;
        },
        asio::detached);

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            auto ec = co_await client.async_open_active(std::move(client_conn));
            TEST_EXPECT_OK(ec);

            // 等待 linktest_loop_ 至少跑一轮并触发断线。
            asio::steady_timer t(ioc);
            t.expires_after(100ms);
            (void)co_await t.async_wait(asio::as_tuple(asio::use_awaitable));

            TEST_EXPECT_EQ(client.state(),
                           secs::hsms::SessionState::disconnected);

            client.stop();
            server_conn.cancel_and_close();
            done = true;
            co_return;
        },
        asio::detached);

    ioc.run();
    TEST_EXPECT(done.load());
}

void test_session_linktest_interval_disconnects_after_threshold() {
    asio::io_context ioc;

    SessionOptions opt;
    opt.session_id = 0x0001;
    opt.t6 = 20ms;
    opt.t7 = 200ms;
    opt.t8 = secs::core::duration{};
    opt.linktest_interval = 30ms;
    opt.linktest_max_consecutive_failures = 3;

    Session client(ioc.get_executor(), opt);

    auto duplex = make_memory_duplex(ioc.get_executor());
    Connection client_conn(std::move(duplex.client_stream),
                           ConnectionOptions{.t8 = secs::core::duration{}});
    Connection server_conn(std::move(duplex.server_stream),
                           ConnectionOptions{.t8 = secs::core::duration{}});

    std::atomic<std::uint32_t> linktests{0};
    std::atomic<bool> done{false};

    // 服务端：select 成功后丢弃所有 linktest.req（不回复），用于制造超时失败。
    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            auto [ec1, m1] = co_await server_conn.async_read_message();
            TEST_EXPECT_OK(ec1);
            TEST_EXPECT_EQ(m1.header.s_type, secs::hsms::SType::select_req);
            ec1 = co_await server_conn.async_write_message(
                secs::hsms::make_select_rsp(
                    m1.header.session_id, 0, m1.header.system_bytes));
            TEST_EXPECT_OK(ec1);

            while (server_conn.is_open()) {
                auto [ec2, m2] = co_await server_conn.async_read_message();
                if (ec2) {
                    break;
                }
                if (m2.header.s_type == secs::hsms::SType::linktest_req) {
                    ++linktests;
                }
            }
            co_return;
        },
        asio::detached);

    asio::co_spawn(
        ioc,
        [&, client_conn = std::move(client_conn)]() mutable
        -> asio::awaitable<void> {
            auto ec = co_await client.async_open_active(std::move(client_conn));
            TEST_EXPECT_OK(ec);

            // 等到第 2 次 linktest.req 发出，并确保第 2 次超时已发生。
            while (linktests.load() < 2U) {
                asio::steady_timer spin(ioc);
                spin.expires_after(1ms);
                (void)co_await spin.async_wait(
                    asio::as_tuple(asio::use_awaitable));
            }
            {
                asio::steady_timer t(ioc);
                t.expires_after(opt.t6 + 10ms);
                (void)co_await t.async_wait(
                    asio::as_tuple(asio::use_awaitable));
            }
            TEST_EXPECT_EQ(client.state(), secs::hsms::SessionState::selected);

            // 第 3 次失败后才断线。
            while (linktests.load() < 3U) {
                asio::steady_timer spin(ioc);
                spin.expires_after(1ms);
                (void)co_await spin.async_wait(
                    asio::as_tuple(asio::use_awaitable));
            }
            {
                asio::steady_timer t(ioc);
                t.expires_after(opt.t6 + 50ms);
                (void)co_await t.async_wait(
                    asio::as_tuple(asio::use_awaitable));
            }
            TEST_EXPECT_EQ(client.state(),
                           secs::hsms::SessionState::disconnected);

            client.stop();
            server_conn.cancel_and_close();
            done = true;
            co_return;
        },
        asio::detached);

    ioc.run();
    TEST_EXPECT(done.load());
}

void test_async_wait_selected_timeout() {
    asio::io_context ioc;

    SessionOptions opt;
    opt.session_id = 0x0001;

    Session client(ioc.get_executor(), opt);

    std::atomic<bool> done{false};
    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            auto ec = co_await client.async_wait_selected(1, 5ms);
            TEST_EXPECT_EQ(ec, make_error_code(errc::timeout));
            done = true;
            co_return;
        },
        asio::detached);

    ioc.run();
    TEST_EXPECT(done.load());
}

void test_session_separate_clears_inbound_and_cancels_pending() {
    asio::io_context ioc;

    SessionOptions opt;
    opt.session_id = 0x0001;
    opt.t3 = 200ms;
    opt.t6 = 50ms;
    opt.t7 = 200ms;
    opt.t8 = secs::core::duration{};

    Session client(ioc.get_executor(), opt);

    auto duplex = make_memory_duplex(ioc.get_executor());
    Connection client_conn(std::move(duplex.client_stream),
                           ConnectionOptions{.t8 = secs::core::duration{}});
    Connection server_conn(std::move(duplex.server_stream),
                           ConnectionOptions{.t8 = secs::core::duration{}});

    std::atomic<bool> done{false};

    // 服务端：
    // 1) select 成功
    // 2) 发送一条未消费的入站 data
    // 3) 读到 client 的挂起请求后，发送 Separate.req 并关闭连接
    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            auto [ec1, m1] = co_await server_conn.async_read_message();
            TEST_EXPECT_OK(ec1);
            TEST_EXPECT_EQ(m1.header.s_type, secs::hsms::SType::select_req);
            ec1 = co_await server_conn.async_write_message(
                secs::hsms::make_select_rsp(
                    m1.header.session_id, 0, m1.header.system_bytes));
            TEST_EXPECT_OK(ec1);

            const std::vector<byte> body = {0xAA, 0xBB};
            auto inbound = secs::hsms::make_data_message(
                opt.session_id,
                1,
                2,
                false,
                0x11111111,
                bytes_view{body.data(), body.size()});
            (void)co_await server_conn.async_write_message(inbound);

            auto [ec2, m2] = co_await server_conn.async_read_message();
            TEST_EXPECT_OK(ec2);
            TEST_EXPECT_EQ(m2.header.s_type, secs::hsms::SType::data);
            TEST_EXPECT(m2.w_bit());

            (void)co_await server_conn.async_write_message(
                secs::hsms::make_separate_req(0xFFFF, 0xABCDEF01));
            (void)co_await server_conn.async_close();
            co_return;
        },
        asio::detached);

    asio::co_spawn(
        ioc,
        [&, client_conn = std::move(client_conn)]() mutable
        -> asio::awaitable<void> {
            auto ec = co_await client.async_open_active(std::move(client_conn));
            TEST_EXPECT_OK(ec);

            const std::vector<byte> req_body = {0x01};
            auto [rec, _] = co_await client.async_request_data(
                1, 1, bytes_view{req_body.data(), req_body.size()});
            TEST_EXPECT(rec.value() != 0);
            TEST_EXPECT(rec != make_error_code(errc::timeout));

            TEST_EXPECT_OK(co_await client.async_wait_reader_stopped(200ms));
            TEST_EXPECT_EQ(client.state(),
                           secs::hsms::SessionState::disconnected);

            // Separate 后必须清空 inbound_data_，否则这里会读到旧数据。
            auto [rec2, _2] = co_await client.async_receive_data(20ms);
            TEST_EXPECT_EQ(rec2, make_error_code(errc::timeout));

            client.stop();
            server_conn.cancel_and_close();
            done = true;
            co_return;
        },
        asio::detached);

    ioc.run();
    TEST_EXPECT(done.load());
}

void test_async_wait_selected_success() {
    asio::io_context ioc;

    SessionOptions opt;
    opt.session_id = 0x0001;
    opt.t6 = 50ms;
    opt.t7 = 200ms;
    opt.t8 = 50ms;

    Session server(ioc.get_executor(), opt);
    Session client(ioc.get_executor(), opt);

    auto duplex = make_memory_duplex(ioc.get_executor());
    Connection client_conn(std::move(duplex.client_stream),
                           ConnectionOptions{.t8 = opt.t8});
    Connection server_conn(std::move(duplex.server_stream),
                           ConnectionOptions{.t8 = opt.t8});

    std::atomic<bool> done{false};

    asio::co_spawn(
        ioc, server.async_open_passive(std::move(server_conn)), asio::detached);

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            auto ec = co_await client.async_open_active(std::move(client_conn));
            TEST_EXPECT_OK(ec);

            ec = co_await client.async_wait_selected(1, 200ms);
            TEST_EXPECT_OK(ec);

            client.stop();
            server.stop();
            done = true;
            co_return;
        },
        asio::detached);

    ioc.run();
    TEST_EXPECT(done.load());
}

void test_session_reopen_after_separate() {
    asio::io_context ioc;

    SessionOptions opt;
    opt.session_id = 0x0001;
    opt.t6 = 50ms;
    opt.t7 = 200ms;
    opt.t8 = 50ms;

    Session server(ioc.get_executor(), opt);
    Session client(ioc.get_executor(), opt);

    std::atomic<bool> done{false};

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            // 第一次连接
            {
                auto duplex = make_memory_duplex(ioc.get_executor());
                Connection client_conn(std::move(duplex.client_stream),
                                       ConnectionOptions{.t8 = opt.t8});
                Connection server_conn(std::move(duplex.server_stream),
                                       ConnectionOptions{.t8 = opt.t8});

                asio::co_spawn(
                    ioc,
                    server.async_open_passive(std::move(server_conn)),
                    asio::detached);
                auto ec =
                    co_await client.async_open_active(std::move(client_conn));
                TEST_EXPECT_OK(ec);
            }

            // 触发断线
            auto ec = co_await server.async_send(secs::hsms::make_separate_req(
                0xFFFF, server.allocate_system_bytes()));
            TEST_EXPECT_OK(ec);

            // 第二次连接（模拟重连）
            {
                auto duplex = make_memory_duplex(ioc.get_executor());
                Connection client_conn(std::move(duplex.client_stream),
                                       ConnectionOptions{.t8 = opt.t8});
                Connection server_conn(std::move(duplex.server_stream),
                                       ConnectionOptions{.t8 = opt.t8});

                asio::co_spawn(
                    ioc,
                    server.async_open_passive(std::move(server_conn)),
                    asio::detached);
                ec = co_await client.async_open_active(std::move(client_conn));
                TEST_EXPECT_OK(ec);
            }

            TEST_EXPECT(client.selected_generation() >= 2);

            client.stop();
            server.stop();
            done = true;
            co_return;
        },
        asio::detached);

    ioc.run();
    TEST_EXPECT(done.load());
}

void test_session_concurrent_sends_system_bytes_unique() {
    asio::io_context ioc;

    SessionOptions opt;
    opt.session_id = 0x0001;
    opt.t6 = 50ms;
    opt.t7 = 200ms;
    opt.t8 = 50ms;

    Session server(ioc.get_executor(), opt);
    Session client(ioc.get_executor(), opt);

    auto duplex = make_memory_duplex(ioc.get_executor());
    Connection client_conn(std::move(duplex.client_stream),
                           ConnectionOptions{.t8 = opt.t8});
    Connection server_conn(std::move(duplex.server_stream),
                           ConnectionOptions{.t8 = opt.t8});

    constexpr int kN = 32;
    std::atomic<int> sent{0};
    std::atomic<bool> received_all{false};
    std::atomic<bool> done{false};

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            auto ec =
                co_await server.async_open_passive(std::move(server_conn));
            TEST_EXPECT_OK(ec);

            std::set<std::uint32_t> seen;
            for (int i = 0; i < kN; ++i) {
                auto [rec, msg] = co_await server.async_receive_data(500ms);
                TEST_EXPECT_OK(rec);
                seen.insert(msg.header.system_bytes);
            }
            TEST_EXPECT_EQ(static_cast<int>(seen.size()), kN);
            received_all = true;
            co_return;
        },
        asio::detached);

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            auto ec = co_await client.async_open_active(std::move(client_conn));
            TEST_EXPECT_OK(ec);

            for (int i = 0; i < kN; ++i) {
                asio::co_spawn(
                    ioc,
                    [&, i]() -> asio::awaitable<void> {
                        const auto sb = client.allocate_system_bytes();
                        const std::array<byte, 1> body = {static_cast<byte>(i)};
                        Message msg = secs::hsms::make_data_message(
                            opt.session_id,
                            1,
                            1,
                            false,
                            sb,
                            bytes_view{body.data(), body.size()});
                        auto ec2 = co_await client.async_send(msg);
                        TEST_EXPECT_OK(ec2);
                        ++sent;
                        co_return;
                    },
                    asio::detached);
            }

            while (sent.load() != kN) {
                asio::steady_timer t(ioc);
                t.expires_after(1ms);
                (void)co_await t.async_wait(
                    asio::as_tuple(asio::use_awaitable));
            }

            while (!received_all.load()) {
                asio::steady_timer t(ioc);
                t.expires_after(1ms);
                (void)co_await t.async_wait(
                    asio::as_tuple(asio::use_awaitable));
            }

            client.stop();
            server.stop();
            done = true;
            co_return;
        },
        asio::detached);

    ioc.run();
    TEST_EXPECT(done.load());
}

void test_run_active_exits_when_auto_reconnect_disabled() {
    asio::io_context ioc;

    SessionOptions opt;
    opt.session_id = 0x0001;
    opt.auto_reconnect = false;
    opt.t5 = 1ms;
    opt.t6 = 10ms;

    Session client(ioc.get_executor(), opt);

    std::atomic<bool> done{false};

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            // 在受限环境下，TCP 连接预计会失败（返回 error_code 而非抛异常）。
            const asio::ip::tcp::endpoint ep{
                asio::ip::make_address("127.0.0.1"), 12345};
            auto ec = co_await client.async_run_active(ep);
            TEST_EXPECT(ec.value() != 0);
            client.stop();
            done = true;
            co_return;
        },
        asio::detached);

    ioc.run();
    TEST_EXPECT(done.load());
}

} // namespace

int main() {
    // 仅用于本地调试定位“卡住在哪个用例”，默认不输出；按需设置：
    // SECS_TEST_TRACE=1 ./build/tests/test_hsms_transport
#define RUN_TEST(fn)                                                           \
    do {                                                                       \
        if (std::getenv("SECS_TEST_TRACE") != nullptr) {                       \
            std::fprintf(stderr, "[hsms_transport] %s\n", #fn);                \
            std::fflush(stderr);                                               \
        }                                                                      \
        fn();                                                                  \
    } while (0)

    RUN_TEST(test_message_encode_decode_roundtrip);
    RUN_TEST(test_encode_frame_rejects_oversized_payload);
    RUN_TEST(test_connection_write_rejects_oversized_message);
    RUN_TEST(test_connection_queue_limit_prioritizes_control);
    RUN_TEST(test_timer_wait_and_cancel);
    RUN_TEST(test_connection_loopback_framing);
    RUN_TEST(test_connection_t8_intercharacter_timeout);
    RUN_TEST(test_connection_t8_disabled);
    RUN_TEST(test_connection_null_stream_and_tcpstream_error_paths);
    RUN_TEST(test_connection_async_read_message_invalid_frames);
    RUN_TEST(test_connection_write_serialization_waiters);
    RUN_TEST(test_session_select_and_linktest);
    RUN_TEST(test_session_select_req_when_already_selected);
    RUN_TEST(test_session_select_req_session_id_mismatch_disconnects);
    RUN_TEST(test_session_deselect_req_moves_to_not_selected);
    RUN_TEST(test_session_select_reject);
    RUN_TEST(test_session_open_passive_socket_overload_executes);
    RUN_TEST(test_session_control_response_type_mismatch_times_out);
    RUN_TEST(test_session_t6_timeout_on_select);
    RUN_TEST(test_session_select_timeout_late_select_rsp_is_ignored);
    RUN_TEST(test_session_pending_cancelled_on_disconnect);
    RUN_TEST(test_session_pending_limit_returns_buffer_overflow);
    RUN_TEST(test_session_deselect_drops_inbound_data_when_not_selected);
    RUN_TEST(test_session_t3_reply_timeout);
    RUN_TEST(test_session_linktest_interval_disconnect_on_failure);
    RUN_TEST(test_session_linktest_interval_disconnects_after_threshold);
    RUN_TEST(test_async_wait_selected_timeout);
    RUN_TEST(test_async_wait_selected_success);
    RUN_TEST(test_session_separate_clears_inbound_and_cancels_pending);
    RUN_TEST(test_session_reopen_after_separate);
    RUN_TEST(test_session_concurrent_sends_system_bytes_unique);
    RUN_TEST(test_run_active_exits_when_auto_reconnect_disabled);

#undef RUN_TEST
    return ::secs::tests::run_and_report();
}
