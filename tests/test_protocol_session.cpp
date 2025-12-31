#include "secs/protocol/router.hpp"
#include "secs/protocol/session.hpp"
#include "secs/protocol/system_bytes.hpp"

#include "secs/core/common.hpp"
#include "secs/core/error.hpp"
#include "secs/core/event.hpp"
#include "secs/hsms/connection.hpp"
#include "secs/hsms/message.hpp"
#include "secs/hsms/session.hpp"
#include "secs/secs1/link.hpp"
#include "secs/secs1/state_machine.hpp"

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
#include <deque>
#include <memory>
#include <optional>
#include <set>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using secs::core::byte;
using secs::core::bytes_view;
using secs::core::errc;
using secs::core::make_error_code;

using secs::protocol::DataMessage;
using secs::protocol::Router;
using secs::protocol::Session;
using secs::protocol::SessionOptions;
using secs::protocol::SystemBytes;

using secs::hsms::Connection;
using secs::hsms::Stream;

using secs::secs1::MemoryLink;
using secs::secs1::StateMachine;
using secs::secs1::Timeouts;

using namespace std::chrono_literals;

// ---------------------------
//  HSMS 内存 Stream（复用 tests/test_hsms_transport.cpp 的模式）
// ---------------------------

struct MemoryChannel final {
    std::deque<byte> buf{};
    bool closed{false};
    secs::core::Event data_event{};
};

class MemoryStream final : public Stream {
public:
    MemoryStream(asio::any_io_executor ex,
                 std::shared_ptr<MemoryChannel> inbox,
                 std::shared_ptr<MemoryChannel> outbox)
        : ex_(std::move(ex)), inbox_(std::move(inbox)),
          outbox_(std::move(outbox)) {}

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
        outbox_->buf.insert(outbox_->buf.end(), src.begin(), src.end());
        outbox_->data_event.set();
        co_return std::error_code{};
    }

    asio::awaitable<std::error_code>
    async_connect(const asio::ip::tcp::endpoint &) override {
        co_return make_error_code(errc::invalid_argument);
    }

private:
    asio::any_io_executor ex_{};
    std::shared_ptr<MemoryChannel> inbox_{};
    std::shared_ptr<MemoryChannel> outbox_{};
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

bytes_view as_bytes(std::string_view s) {
    return bytes_view{reinterpret_cast<const byte *>(s.data()), s.size()};
}

// ---------------------------
//  单元测试：SystemBytes / Router
// ---------------------------

void test_system_bytes_unique_release_reuse_and_wrap() {
    SystemBytes sb(1U);

    std::uint32_t a = 0;
    std::uint32_t b = 0;
    std::uint32_t c = 0;
    TEST_EXPECT_OK(sb.allocate(a));
    TEST_EXPECT_OK(sb.allocate(b));
    TEST_EXPECT_OK(sb.allocate(c));
    TEST_EXPECT(a != 0U);
    TEST_EXPECT(b != 0U);
    TEST_EXPECT(c != 0U);
    TEST_EXPECT(a != b);
    TEST_EXPECT(a != c);
    TEST_EXPECT(b != c);

    TEST_EXPECT(sb.is_in_use(a));
    TEST_EXPECT(sb.is_in_use(b));
    TEST_EXPECT(sb.is_in_use(c));
    TEST_EXPECT_EQ(sb.in_use_count(), 3U);

    sb.release(b);
    TEST_EXPECT(!sb.is_in_use(b));
    TEST_EXPECT_EQ(sb.in_use_count(), 2U);

    std::uint32_t b2 = 0;
    TEST_EXPECT_OK(sb.allocate(b2));
    TEST_EXPECT_EQ(b2, b); // 释放后应可重用
    TEST_EXPECT_EQ(sb.in_use_count(), 3U);

    sb.release(a);
    sb.release(b2);
    sb.release(c);
    TEST_EXPECT_EQ(sb.in_use_count(), 0U);

    // 非法释放值（0）应被忽略；释放未在用的值也应被忽略。
    sb.release(0U);
    sb.release(0xA5A5A5A5U);
    TEST_EXPECT_EQ(sb.in_use_count(), 0U);

    // 初始值为 0 时应归一化到 1。
    SystemBytes sb0(0U);
    std::uint32_t z = 0;
    TEST_EXPECT_OK(sb0.allocate(z));
    TEST_EXPECT_EQ(z, 1U);

    // 回绕：从 0xFFFFFFFE 开始分配应能回到 1（跳过 0）。
    SystemBytes sb2(0xFFFFFFFEU);
    std::uint32_t x1 = 0;
    std::uint32_t x2 = 0;
    std::uint32_t x3 = 0;
    TEST_EXPECT_OK(sb2.allocate(x1));
    TEST_EXPECT_OK(sb2.allocate(x2));
    TEST_EXPECT_OK(sb2.allocate(x3));
    TEST_EXPECT_EQ(x1, 0xFFFFFFFEU);
    TEST_EXPECT_EQ(x2, 0xFFFFFFFFU);
    TEST_EXPECT_EQ(x3, 1U);
}

void test_router_set_find_erase_clear() {
    Router r;
    TEST_EXPECT(!r.find(1, 1).has_value());

    r.set(1,
          1,
          [](const DataMessage &)
              -> asio::awaitable<secs::protocol::HandlerResult> {
              co_return secs::protocol::HandlerResult{std::error_code{},
                                                      std::vector<byte>{0xAA}};
          });
    TEST_EXPECT(r.find(1, 1).has_value());

    r.erase(1, 1);
    TEST_EXPECT(!r.find(1, 1).has_value());

    r.set(2,
          3,
          [](const DataMessage &)
              -> asio::awaitable<secs::protocol::HandlerResult> {
              co_return secs::protocol::HandlerResult{std::error_code{},
                                                      std::vector<byte>{}};
          });
    r.clear();
    TEST_EXPECT(!r.find(2, 3).has_value());
}

// ---------------------------
//  集成测试
// ---------------------------

void test_hsms_protocol_pending_filters() {
    asio::io_context ioc;
    const std::uint16_t session_id = 0x1010;

    secs::core::Event server_opened{};
    secs::core::Event client_opened{};

    secs::hsms::Session server(ioc.get_executor(),
                               secs::hsms::SessionOptions{
                                   .session_id = session_id,
                                   .t3 = 200ms,
                                   .t5 = 10ms,
                                   .t6 = 50ms,
                                   .t7 = 50ms,
                                   .t8 = 0ms,
                                   .linktest_interval = 0ms,
                                   .auto_reconnect = false,
                               });

    secs::hsms::Session client(ioc.get_executor(),
                               secs::hsms::SessionOptions{
                                   .session_id = session_id,
                                   .t3 = 200ms,
                                   .t5 = 10ms,
                                   .t6 = 50ms,
                                   .t7 = 50ms,
                                   .t8 = 0ms,
                                   .linktest_interval = 0ms,
                                   .auto_reconnect = false,
                               });

    SessionOptions proto_opts{};
    proto_opts.t3 = 200ms;
    proto_opts.poll_interval = 1ms;

    Session proto_server(server, session_id, proto_opts);
    Session proto_client(client, session_id, proto_opts);

    // 服务端处理器：在回复前插入两条“SystemBytes
    // 相同但不应命中挂起请求”的消息： 1) 应答消息（secondary）但 Function
    // 号不匹配 2) 请求消息（primary，Function 为奇数）且 W 位=true
    proto_server.router().set(
        1,
        1,
        [&](const DataMessage &msg)
            -> asio::awaitable<secs::protocol::HandlerResult> {
            {
                const auto wrong_secondary =
                    secs::hsms::make_data_message(session_id,
                                                  msg.stream,
                                                  4,
                                                  false,
                                                  msg.system_bytes,
                                                  as_bytes("wrong-secondary"));
                TEST_EXPECT_OK(co_await server.async_send(wrong_secondary));
            }
            {
                const auto collide_primary =
                    secs::hsms::make_data_message(session_id,
                                                  3,
                                                  3,
                                                  true,
                                                  msg.system_bytes,
                                                  as_bytes("collide-primary"));
                TEST_EXPECT_OK(co_await server.async_send(collide_primary));
            }
            co_return secs::protocol::HandlerResult{std::error_code{},
                                                    msg.body};
        });

    auto duplex = make_memory_duplex(ioc.get_executor());
    Connection server_conn(std::move(duplex.server_stream));
    Connection client_conn(std::move(duplex.client_stream));

    asio::co_spawn(
        ioc,
        [&, server_conn = std::move(server_conn)]() mutable
        -> asio::awaitable<void> {
            auto ec =
                co_await server.async_open_passive(std::move(server_conn));
            TEST_EXPECT_OK(ec);
            server_opened.set();
        },
        asio::detached);

    asio::co_spawn(
        ioc,
        [&, client_conn = std::move(client_conn)]() mutable
        -> asio::awaitable<void> {
            auto ec = co_await client.async_open_active(std::move(client_conn));
            TEST_EXPECT_OK(ec);
            client_opened.set();
        },
        asio::detached);

    std::atomic<bool> done{false};
    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            TEST_EXPECT_OK(co_await server_opened.async_wait(200ms));
            TEST_EXPECT_OK(co_await client_opened.async_wait(200ms));

            asio::co_spawn(ioc, proto_server.async_run(), asio::detached);
            asio::co_spawn(ioc, proto_client.async_run(), asio::detached);

            std::vector<byte> payload = {0x01, 0x02, 0x03};
            auto [ec, rsp] = co_await proto_client.async_request(
                1, 1, bytes_view{payload.data(), payload.size()});
            TEST_EXPECT_OK(ec);
            TEST_EXPECT_EQ(rsp.stream, 1);
            TEST_EXPECT_EQ(rsp.function, 2);
            TEST_EXPECT(
                std::equal(rsp.body.begin(), rsp.body.end(), payload.begin()));

            proto_server.stop();
            proto_client.stop();
            client.stop();
            server.stop();
            done = true;
        },
        asio::detached);

    ioc.run();
    TEST_EXPECT(done);
}

void test_hsms_protocol_stop_cancels_pending() {
    asio::io_context ioc;
    const std::uint16_t session_id = 0x1011;

    secs::core::Event server_opened{};
    secs::core::Event client_opened{};

    secs::hsms::Session server(ioc.get_executor(),
                               secs::hsms::SessionOptions{
                                   .session_id = session_id,
                                   .t3 = 200ms,
                                   .t5 = 10ms,
                                   .t6 = 50ms,
                                   .t7 = 50ms,
                                   .t8 = 0ms,
                                   .linktest_interval = 0ms,
                                   .auto_reconnect = false,
                               });

    secs::hsms::Session client(ioc.get_executor(),
                               secs::hsms::SessionOptions{
                                   .session_id = session_id,
                                   .t3 = 200ms,
                                   .t5 = 10ms,
                                   .t6 = 50ms,
                                   .t7 = 50ms,
                                   .t8 = 0ms,
                                   .linktest_interval = 0ms,
                                   .auto_reconnect = false,
                               });

    Session proto_client(
        client, session_id, SessionOptions{.t3 = 200ms, .poll_interval = 1ms});

    auto duplex = make_memory_duplex(ioc.get_executor());
    Connection server_conn(std::move(duplex.server_stream));
    Connection client_conn(std::move(duplex.client_stream));

    asio::co_spawn(
        ioc,
        [&, server_conn = std::move(server_conn)]() mutable
        -> asio::awaitable<void> {
            auto ec =
                co_await server.async_open_passive(std::move(server_conn));
            TEST_EXPECT_OK(ec);
            server_opened.set();
        },
        asio::detached);

    asio::co_spawn(
        ioc,
        [&, client_conn = std::move(client_conn)]() mutable
        -> asio::awaitable<void> {
            auto ec = co_await client.async_open_active(std::move(client_conn));
            TEST_EXPECT_OK(ec);
            client_opened.set();
        },
        asio::detached);

    std::atomic<bool> done{false};
    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            TEST_EXPECT_OK(co_await server_opened.async_wait(200ms));
            TEST_EXPECT_OK(co_await client_opened.async_wait(200ms));

            // 发起一个会挂起的请求，然后由 stop() 取消（覆盖
            // cancel_all_pending_ 分支）。
            auto [ec, rsp] = co_await proto_client.async_request(
                1, 1, as_bytes("will-cancel"), 200ms);
            (void)rsp;
            TEST_EXPECT_EQ(ec, make_error_code(errc::cancelled));
            done = true;
        },
        asio::detached);

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            TEST_EXPECT_OK(co_await server_opened.async_wait(200ms));
            TEST_EXPECT_OK(co_await client_opened.async_wait(200ms));

            asio::steady_timer t(ioc.get_executor());
            t.expires_after(5ms);
            (void)co_await t.async_wait(asio::as_tuple(asio::use_awaitable));
            proto_client.stop();
            client.stop();
            server.stop();
        },
        asio::detached);

    ioc.run();
    TEST_EXPECT(done);
}

void test_hsms_protocol_run_without_poll_interval() {
    asio::io_context ioc;
    const std::uint16_t session_id = 0x1012;

    secs::core::Event server_opened{};
    secs::core::Event client_opened{};

    secs::hsms::Session server(ioc.get_executor(),
                               secs::hsms::SessionOptions{
                                   .session_id = session_id,
                                   .t3 = 200ms,
                                   .t5 = 10ms,
                                   .t6 = 50ms,
                                   .t7 = 50ms,
                                   .t8 = 0ms,
                                   .linktest_interval = 0ms,
                                   .auto_reconnect = false,
                               });

    secs::hsms::Session client(ioc.get_executor(),
                               secs::hsms::SessionOptions{
                                   .session_id = session_id,
                                   .t3 = 200ms,
                                   .t5 = 10ms,
                                   .t6 = 50ms,
                                   .t7 = 50ms,
                                   .t8 = 0ms,
                                   .linktest_interval = 0ms,
                                   .auto_reconnect = false,
                               });

    Session proto_server(
        server, session_id, SessionOptions{.t3 = 200ms, .poll_interval = 0ms});
    Session proto_client(
        client, session_id, SessionOptions{.t3 = 200ms, .poll_interval = 1ms});

    // 处理器：用于覆盖 normalize_timeout(d==0) 分支（poll_interval=0
    // 会被归一化为 std::nullopt，即“无超时等待”）。
    proto_server.router().set(
        1,
        1,
        [&](const DataMessage &msg)
            -> asio::awaitable<secs::protocol::HandlerResult> {
            co_return secs::protocol::HandlerResult{std::error_code{},
                                                    msg.body};
        });

    auto duplex = make_memory_duplex(ioc.get_executor());
    Connection server_conn(std::move(duplex.server_stream));
    Connection client_conn(std::move(duplex.client_stream));

    asio::co_spawn(
        ioc,
        [&, server_conn = std::move(server_conn)]() mutable
        -> asio::awaitable<void> {
            auto ec =
                co_await server.async_open_passive(std::move(server_conn));
            TEST_EXPECT_OK(ec);
            server_opened.set();
        },
        asio::detached);

    asio::co_spawn(
        ioc,
        [&, client_conn = std::move(client_conn)]() mutable
        -> asio::awaitable<void> {
            auto ec = co_await client.async_open_active(std::move(client_conn));
            TEST_EXPECT_OK(ec);
            client_opened.set();
        },
        asio::detached);

    std::atomic<bool> done{false};
    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            TEST_EXPECT_OK(co_await server_opened.async_wait(200ms));
            TEST_EXPECT_OK(co_await client_opened.async_wait(200ms));

            asio::co_spawn(ioc, proto_server.async_run(), asio::detached);

            auto [ec, rsp] =
                co_await proto_client.async_request(1, 1, as_bytes("once"));
            TEST_EXPECT_OK(ec);
            TEST_EXPECT_EQ(rsp.function, 2);

            proto_client.stop();
            client.stop();
            server.stop();
            done = true;
        },
        asio::detached);

    ioc.run();
    TEST_EXPECT(done);
}

void test_hsms_protocol_echo_1000() {
    asio::io_context ioc;
    const std::uint16_t session_id = 0x1001;

    secs::core::Event server_opened{};
    secs::core::Event client_opened{};

    secs::hsms::Session server(ioc.get_executor(),
                               secs::hsms::SessionOptions{
                                   .session_id = session_id,
                                   .t3 = 200ms,
                                   .t5 = 10ms,
                                   .t6 = 50ms,
                                   .t7 = 50ms,
                                   .t8 = 0ms,
                                   .linktest_interval = 0ms,
                                   .auto_reconnect = false,
                               });

    secs::hsms::Session client(ioc.get_executor(),
                               secs::hsms::SessionOptions{
                                   .session_id = session_id,
                                   .t3 = 200ms,
                                   .t5 = 10ms,
                                   .t6 = 50ms,
                                   .t7 = 50ms,
                                   .t8 = 0ms,
                                   .linktest_interval = 0ms,
                                   .auto_reconnect = false,
                               });

    SessionOptions proto_opts{};
    proto_opts.t3 = 200ms;
    proto_opts.poll_interval = 1ms;

    Session proto_server(server, session_id, proto_opts);
    Session proto_client(client, session_id, proto_opts);

    std::atomic<std::size_t> handled{0};
    proto_server.router().set(
        1,
        1,
        [&](const DataMessage &msg)
            -> asio::awaitable<secs::protocol::HandlerResult> {
            ++handled;
            co_return secs::protocol::HandlerResult{std::error_code{},
                                                    msg.body};
        });

    auto duplex = make_memory_duplex(ioc.get_executor());
    Connection server_conn(std::move(duplex.server_stream));
    Connection client_conn(std::move(duplex.client_stream));

    asio::co_spawn(
        ioc,
        [&, server_conn = std::move(server_conn)]() mutable
        -> asio::awaitable<void> {
            auto ec =
                co_await server.async_open_passive(std::move(server_conn));
            TEST_EXPECT_OK(ec);
            server_opened.set();
        },
        asio::detached);

    asio::co_spawn(
        ioc,
        [&, client_conn = std::move(client_conn)]() mutable
        -> asio::awaitable<void> {
            auto ec = co_await client.async_open_active(std::move(client_conn));
            TEST_EXPECT_OK(ec);
            client_opened.set();
        },
        asio::detached);

    std::atomic<bool> done{false};
    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            TEST_EXPECT_OK(co_await server_opened.async_wait(200ms));
            TEST_EXPECT_OK(co_await client_opened.async_wait(200ms));

            // 服务端接收循环负责接收并回包
            asio::co_spawn(ioc, proto_server.async_run(), asio::detached);

            // 客户端接收循环可由 async_request
            // 自动拉起；这里显式启动一次以覆盖“重复 run”分支
            asio::co_spawn(ioc, proto_client.async_run(), asio::detached);

            for (std::size_t i = 0; i < 1000; ++i) {
                std::vector<byte> payload = {
                    static_cast<byte>(i & 0xFFU),
                    static_cast<byte>((i >> 8U) & 0xFFU)};
                auto [ec, rsp] = co_await proto_client.async_request(
                    1, 1, bytes_view{payload.data(), payload.size()});
                TEST_EXPECT_OK(ec);
                TEST_EXPECT_EQ(rsp.stream, 1);
                TEST_EXPECT_EQ(rsp.function, 2);
                TEST_EXPECT(!rsp.w_bit);
                TEST_EXPECT(rsp.system_bytes != 0U);
                TEST_EXPECT_EQ(rsp.body.size(), payload.size());
                TEST_EXPECT(std::equal(
                    rsp.body.begin(), rsp.body.end(), payload.begin()));
            }

            proto_server.stop();
            proto_client.stop();
            client.stop();
            server.stop();
            done = true;
        },
        asio::detached);

    ioc.run();

    TEST_EXPECT(done);
    TEST_EXPECT_EQ(handled.load(), 1000U);
}

void test_hsms_protocol_t3_timeout() {
    asio::io_context ioc;
    const std::uint16_t session_id = 0x1002;

    secs::core::Event server_opened{};
    secs::core::Event client_opened{};

    secs::hsms::Session server(ioc.get_executor(),
                               secs::hsms::SessionOptions{
                                   .session_id = session_id,
                                   .t3 = 200ms,
                                   .t5 = 10ms,
                                   .t6 = 50ms,
                                   .t7 = 50ms,
                                   .t8 = 0ms,
                                   .linktest_interval = 0ms,
                                   .auto_reconnect = false,
                               });

    secs::hsms::Session client(ioc.get_executor(),
                               secs::hsms::SessionOptions{
                                   .session_id = session_id,
                                   .t3 = 200ms,
                                   .t5 = 10ms,
                                   .t6 = 50ms,
                                   .t7 = 50ms,
                                   .t8 = 0ms,
                                   .linktest_interval = 0ms,
                                   .auto_reconnect = false,
                               });

    SessionOptions proto_opts{};
    proto_opts.t3 = 20ms;
    proto_opts.poll_interval = 1ms;

    Session proto_client(client, session_id, proto_opts);

    auto duplex = make_memory_duplex(ioc.get_executor());
    Connection server_conn(std::move(duplex.server_stream));
    Connection client_conn(std::move(duplex.client_stream));

    asio::co_spawn(
        ioc,
        [&, server_conn = std::move(server_conn)]() mutable
        -> asio::awaitable<void> {
            auto ec =
                co_await server.async_open_passive(std::move(server_conn));
            TEST_EXPECT_OK(ec);
            server_opened.set();
        },
        asio::detached);

    asio::co_spawn(
        ioc,
        [&, client_conn = std::move(client_conn)]() mutable
        -> asio::awaitable<void> {
            auto ec = co_await client.async_open_active(std::move(client_conn));
            TEST_EXPECT_OK(ec);
            client_opened.set();
        },
        asio::detached);

    std::atomic<bool> done{false};
    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            TEST_EXPECT_OK(co_await server_opened.async_wait(200ms));
            TEST_EXPECT_OK(co_await client_opened.async_wait(200ms));

            // 不启动协议层服务端、不注册处理器 -> 客户端应触发 T3 超时
            auto [ec, rsp] =
                co_await proto_client.async_request(1, 1, as_bytes("ping"));
            (void)rsp;
            TEST_EXPECT_EQ(ec, make_error_code(errc::timeout));

            proto_client.stop();
            client.stop();
            server.stop();
            done = true;
        },
        asio::detached);

    ioc.run();
    TEST_EXPECT(done);
}

void test_secs1_protocol_echo_100() {
    asio::io_context ioc;

    auto [a, b] = MemoryLink::create(ioc.get_executor());

    Timeouts timeouts{};
    timeouts.t1_intercharacter = 50ms;
    timeouts.t2_protocol = 100ms;
    timeouts.t3_reply = 200ms;
    timeouts.t4_interblock = 100ms;

    constexpr std::uint16_t device_id = 0x0001;
    StateMachine sm_a(a, device_id, timeouts);
    StateMachine sm_b(b, device_id, timeouts);

    SessionOptions proto_opts{};
    proto_opts.t3 = 200ms;
    proto_opts.poll_interval = 1ms;

    Session proto_server(sm_b, device_id, proto_opts);
    Session proto_client(sm_a, device_id, proto_opts);

    std::atomic<std::size_t> handled{0};
    proto_server.router().set(
        2,
        3,
        [&](const DataMessage &msg)
            -> asio::awaitable<secs::protocol::HandlerResult> {
            ++handled;
            // 这里做一个简单变换：回包在末尾追加 1B，覆盖非空消息体分支
            std::vector<byte> out = msg.body;
            out.push_back(static_cast<byte>(0xEE));
            co_return secs::protocol::HandlerResult{std::error_code{},
                                                    std::move(out)};
        });

    asio::co_spawn(ioc, proto_server.async_run(), asio::detached);

    std::atomic<bool> done{false};
    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            for (std::size_t i = 0; i < 100; ++i) {
                std::vector<byte> payload = {static_cast<byte>(i & 0xFFU)};
                auto [ec, rsp] = co_await proto_client.async_request(
                    2, 3, bytes_view{payload.data(), payload.size()});
                TEST_EXPECT_OK(ec);
                TEST_EXPECT_EQ(rsp.stream, 2);
                TEST_EXPECT_EQ(rsp.function, 4);
                TEST_EXPECT(!rsp.w_bit);
                TEST_EXPECT(rsp.system_bytes != 0U);
                TEST_EXPECT_EQ(rsp.body.size(), payload.size() + 1U);
                TEST_EXPECT_EQ(rsp.body[0], payload[0]);
                TEST_EXPECT_EQ(rsp.body.back(), static_cast<byte>(0xEE));
            }

            proto_server.stop();
            proto_client.stop();
            done = true;
        },
        asio::detached);

    ioc.run();

    TEST_EXPECT(done);
    TEST_EXPECT_EQ(handled.load(), 100U);
}

void test_session_invalid_arguments() {
    asio::io_context ioc;
    const std::uint16_t session_id = 0x2001;

    secs::hsms::Session client(
        ioc.get_executor(),
        secs::hsms::SessionOptions{.session_id = session_id,
                                   .auto_reconnect = false});
    Session proto_client(
        client, session_id, SessionOptions{.t3 = 10ms, .poll_interval = 1ms});

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            auto [ec0, rsp0] =
                co_await proto_client.async_request(1, 0, as_bytes("x"));
            (void)rsp0;
            TEST_EXPECT_EQ(ec0, make_error_code(errc::invalid_argument));

            auto [ec1, rsp1] =
                co_await proto_client.async_request(1, 2, as_bytes("x"));
            (void)rsp1;
            TEST_EXPECT_EQ(ec1, make_error_code(errc::invalid_argument));

            auto [ec2, rsp2] =
                co_await proto_client.async_request(1, 0xFF, as_bytes("x"));
            (void)rsp2;
            TEST_EXPECT_EQ(ec2, make_error_code(errc::invalid_argument));

            auto ec3 = co_await proto_client.async_send(200, 1, as_bytes("x"));
            TEST_EXPECT_EQ(ec3, make_error_code(errc::invalid_argument));
        },
        asio::detached);

    ioc.run();
}

} // namespace

int main() {
    test_system_bytes_unique_release_reuse_and_wrap();
    test_router_set_find_erase_clear();
    test_hsms_protocol_pending_filters();
    test_hsms_protocol_stop_cancels_pending();
    test_hsms_protocol_run_without_poll_interval();
    test_hsms_protocol_echo_1000();
    test_hsms_protocol_t3_timeout();
    test_secs1_protocol_echo_100();
    test_session_invalid_arguments();
    return secs::tests::run_and_report();
}
