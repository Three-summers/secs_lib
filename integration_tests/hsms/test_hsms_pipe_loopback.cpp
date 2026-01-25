#include "secs/core/common.hpp"
#include "secs/core/error.hpp"
#include "secs/core/event.hpp"
#
#include "secs/hsms/connection.hpp"
#include "secs/hsms/message.hpp"
#include "secs/hsms/session.hpp"
#
#include "secs/ii/codec.hpp"
#include "secs/ii/item.hpp"
#
#include <asio/as_tuple.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/posix/stream_descriptor.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>
#
#include <signal.h>
#include <unistd.h>
#
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#
namespace {
#
using secs::core::byte;
using secs::core::bytes_view;
using secs::core::errc;
using secs::core::make_error_code;
#
using namespace std::chrono_literals;
#
struct UniqueFd final {
    int fd{-1};
    UniqueFd() = default;
    explicit UniqueFd(int v) : fd(v) {}
    UniqueFd(const UniqueFd &) = delete;
    UniqueFd &operator=(const UniqueFd &) = delete;
    UniqueFd(UniqueFd &&o) noexcept : fd(o.fd) { o.fd = -1; }
    UniqueFd &operator=(UniqueFd &&o) noexcept {
        if (this == &o) {
            return *this;
        }
        reset();
        fd = o.fd;
        o.fd = -1;
        return *this;
    }
    ~UniqueFd() { reset(); }
#
    void reset() noexcept {
        if (fd >= 0) {
            (void)::close(fd);
        }
        fd = -1;
    }
#
    [[nodiscard]] int release() noexcept {
        const int v = fd;
        fd = -1;
        return v;
    }
};
#
static std::error_code make_errno_ec() noexcept {
    return std::error_code(errno, std::generic_category());
}
#
class PipeStream final : public secs::hsms::Stream {
public:
    PipeStream(asio::any_io_executor ex, int read_fd, int write_fd)
        : ex_(ex), reader_(ex, read_fd), writer_(ex, write_fd) {}
#
    [[nodiscard]] asio::any_io_executor executor() const noexcept override {
        return ex_;
    }
    [[nodiscard]] bool is_open() const noexcept override { return open_; }
#
    void cancel() noexcept override {
        std::error_code ignored;
        reader_.cancel(ignored);
        writer_.cancel(ignored);
    }
#
    void close() noexcept override {
        if (!open_) {
            return;
        }
        open_ = false;
        std::error_code ignored;
        reader_.close(ignored);
        writer_.close(ignored);
    }
#
    asio::awaitable<std::pair<std::error_code, std::size_t>>
    async_read_some(secs::core::mutable_bytes_view dst) override {
        auto [ec, n] = co_await reader_.async_read_some(
            asio::buffer(dst.data(), dst.size()),
            asio::as_tuple(asio::use_awaitable));
        co_return std::pair{ec, n};
    }
#
    asio::awaitable<std::error_code>
    async_write_all(bytes_view src) override {
        auto [ec, n] =
            co_await asio::async_write(writer_,
                                       asio::buffer(src.data(), src.size()),
                                       asio::as_tuple(asio::use_awaitable));
        if (ec) {
            co_return ec;
        }
        if (n != src.size()) {
            co_return make_error_code(errc::invalid_argument);
        }
        co_return std::error_code{};
    }
#
    asio::awaitable<std::error_code>
    async_connect(const asio::ip::tcp::endpoint &) override {
        co_return make_error_code(errc::invalid_argument);
    }
#
private:
    asio::any_io_executor ex_;
    asio::posix::stream_descriptor reader_;
    asio::posix::stream_descriptor writer_;
    bool open_{true};
};
#
static secs::ii::Item make_test_item(std::uint32_t tag) {
    return secs::ii::Item::list({
        secs::ii::Item::u4({tag}),
        secs::ii::Item::ascii("HELLO"),
        secs::ii::Item::list({
            secs::ii::Item::u1({1U, 2U, 3U}),
        }),
    });
}
#
static std::pair<std::error_code, std::vector<byte>>
encode_item(const secs::ii::Item &item) {
    std::vector<byte> out;
    if (auto ec = secs::ii::encode(item, out); ec) {
        return {ec, {}};
    }
    return {std::error_code{}, std::move(out)};
}
#
static std::pair<std::error_code, secs::ii::Item>
decode_item(bytes_view body) {
    secs::ii::Item item = secs::ii::Item::binary({});
    std::size_t consumed = 0;
    auto ec = secs::ii::decode_one(body, item, consumed);
    if (ec) {
        return {ec, secs::ii::Item::binary({})};
    }
    if (consumed != body.size()) {
        return {secs::core::make_error_code(secs::core::errc::invalid_argument),
                secs::ii::Item::binary({})};
    }
    return {std::error_code{}, std::move(item)};
}
#
static secs::hsms::SessionOptions make_default_opts(std::uint16_t device_id) {
    secs::hsms::SessionOptions options{};
    options.session_id = device_id;
    options.t3 = 800ms;
    options.t6 = 800ms;
    options.t7 = 800ms;
    options.t8 = 200ms;
    options.auto_reconnect = false;
    return options;
}
#
struct PipeDuplex final {
    std::unique_ptr<secs::hsms::Stream> client_stream;
    std::unique_ptr<secs::hsms::Stream> server_stream;
};
#
static std::pair<std::error_code, PipeDuplex>
make_pipe_duplex(asio::any_io_executor client_ex,
                 asio::any_io_executor server_ex) {
    int c2s[2]{-1, -1};
    int s2c[2]{-1, -1};
    if (::pipe(c2s) != 0) {
        return {make_errno_ec(), PipeDuplex{}};
    }
    if (::pipe(s2c) != 0) {
        (void)::close(c2s[0]);
        (void)::close(c2s[1]);
        return {make_errno_ec(), PipeDuplex{}};
    }
#
    // client: 读 s2c[0]，写 c2s[1]
    // server: 读 c2s[0]，写 s2c[1]
    UniqueFd c2s_r(c2s[0]);
    UniqueFd c2s_w(c2s[1]);
    UniqueFd s2c_r(s2c[0]);
    UniqueFd s2c_w(s2c[1]);
#
    PipeDuplex duplex;
    duplex.client_stream = std::make_unique<PipeStream>(
        client_ex, s2c_r.release(), c2s_w.release());
    duplex.server_stream = std::make_unique<PipeStream>(
        server_ex, c2s_r.release(), s2c_w.release());
#
    return {std::error_code{}, std::move(duplex)};
}
#
static asio::awaitable<int> case_basic_request_response() {
    auto ex = co_await asio::this_coro::executor;
    constexpr std::uint16_t device_id = 1;
#
    const auto server_opts = make_default_opts(device_id);
    auto client_opts = make_default_opts(device_id);
#
    secs::hsms::Session server(ex, server_opts);
    secs::hsms::Session client(ex, client_opts);
#
    auto [duplex_ec, duplex] =
        make_pipe_duplex(client.executor(), server.executor());
    if (duplex_ec) {
        std::cerr << "[HSMS][pipe][basic] 创建 pipe 失败: " << duplex_ec.message()
                  << "\n";
        co_return 2;
    }
#
    secs::hsms::Connection client_conn(
        std::move(duplex.client_stream),
        secs::hsms::ConnectionOptions{.t8 = client_opts.t8});
    secs::hsms::Connection server_conn(
        std::move(duplex.server_stream),
        secs::hsms::ConnectionOptions{.t8 = server_opts.t8});
#
    secs::core::Event server_done{};
    int server_rc = 1;
    int rc = 0;
#
    asio::co_spawn(
        ex,
        [&]() -> asio::awaitable<void> {
            auto ec = co_await server.async_open_passive(std::move(server_conn));
            if (ec) {
                server_rc = 10;
                std::cerr << "[HSMS][pipe][basic] server SELECT 失败: "
                          << ec.message() << "\n";
                server.stop();
                server_done.set();
                co_return;
            }
#
            auto [rec, req] = co_await server.async_receive_data(2s);
            if (rec) {
                server_rc = 11;
                std::cerr << "[HSMS][pipe][basic] server recv 失败: "
                          << rec.message() << "\n";
                server.stop();
                server_done.set();
                co_return;
            }
#
            auto [dec_ec, _] = decode_item(
                bytes_view{req.body.data(), req.body.size()});
            if (dec_ec) {
                server_rc = 12;
                std::cerr << "[HSMS][pipe][basic] server 解码失败: "
                          << dec_ec.message() << "\n";
                server.stop();
                server_done.set();
                co_return;
            }
#
            const auto rsp =
                secs::hsms::make_data_message(server_opts.session_id,
                                              /*stream=*/1,
                                              /*function=*/14,
                                              /*w_bit=*/false,
                                              req.header.system_bytes,
                                              bytes_view{req.body.data(),
                                                         req.body.size()});
            ec = co_await server.async_send(rsp);
            if (ec) {
                server_rc = 13;
                std::cerr << "[HSMS][pipe][basic] server send 失败: "
                          << ec.message() << "\n";
                server.stop();
                server_done.set();
                co_return;
            }
#
            server.stop();
            server_rc = 0;
            server_done.set();
            co_return;
        },
        asio::detached);
#
    auto ec = co_await client.async_open_active(std::move(client_conn));
    if (ec) {
        std::cerr << "[HSMS][pipe][basic] client SELECT 失败: " << ec.message()
                  << "\n";
        rc = 4;
    }
#
    if (rc == 0) {
        ec = co_await client.async_linktest();
        if (ec) {
            std::cerr << "[HSMS][pipe][basic] client LINKTEST 失败: " << ec.message()
                      << "\n";
            rc = 5;
        }
    }
#
    if (rc == 0) {
        const auto item = make_test_item(/*tag=*/123U);
        auto enc = encode_item(item);
        if (enc.first) {
            std::cerr << "[HSMS][pipe][basic] client 编码失败: " << enc.first.message()
                      << "\n";
            rc = 6;
        } else {
            auto req = co_await client.async_request_data(
                1,
                13,
                bytes_view{enc.second.data(), enc.second.size()},
                800ms);
            if (req.first) {
                std::cerr << "[HSMS][pipe][basic] client request 失败: "
                          << req.first.message() << "\n";
                rc = 7;
            } else if (req.second.stream() != 1 || req.second.function() != 14 ||
                       req.second.w_bit()) {
                std::cerr << "[HSMS][pipe][basic] 响应头不符合预期: stream="
                          << static_cast<int>(req.second.stream())
                          << " function=" << static_cast<int>(req.second.function())
                          << " w=" << (req.second.w_bit() ? 1 : 0) << "\n";
                rc = 8;
            } else {
                auto dec = decode_item(
                    bytes_view{req.second.body.data(), req.second.body.size()});
                if (dec.first) {
                    std::cerr << "[HSMS][pipe][basic] client 解码失败: "
                              << dec.first.message() << "\n";
                    rc = 9;
                } else if (dec.second != item) {
                    std::cerr << "[HSMS][pipe][basic] 响应 Item 与请求不一致\n";
                    rc = 10;
                }
            }
        }
    }
#
    client.stop();
    (void)co_await client.async_wait_reader_stopped(1s);
    server.stop();
#
    ec = co_await server_done.async_wait(2s);
    if (ec) {
        std::cerr << "[HSMS][pipe][basic] 等待 server_done 失败: " << ec.message()
                  << "\n";
        if (rc == 0) {
            rc = 11;
        }
    }
    (void)co_await server.async_wait_reader_stopped(1s);
#
    if (rc == 0 && server_rc != 0) {
        rc = server_rc;
    }
    co_return rc;
}
#
static asio::awaitable<int> case_out_of_order_responses() {
    auto ex = co_await asio::this_coro::executor;
    constexpr std::uint16_t device_id = 1;
#
    const auto server_opts = make_default_opts(device_id);
    auto client_opts = make_default_opts(device_id);
#
    secs::hsms::Session server(ex, server_opts);
    secs::hsms::Session client(ex, client_opts);
#
    auto [duplex_ec, duplex] =
        make_pipe_duplex(client.executor(), server.executor());
    if (duplex_ec) {
        std::cerr << "[HSMS][pipe][reorder] 创建 pipe 失败: " << duplex_ec.message()
                  << "\n";
        co_return 2;
    }
#
    secs::hsms::Connection client_conn(
        std::move(duplex.client_stream),
        secs::hsms::ConnectionOptions{.t8 = client_opts.t8});
    secs::hsms::Connection server_conn(
        std::move(duplex.server_stream),
        secs::hsms::ConnectionOptions{.t8 = server_opts.t8});
#
    secs::core::Event server_done{};
    int server_rc = 1;
    int rc = 0;
#
    struct Shared final {
        secs::core::Event done1{};
        secs::core::Event done2{};
        int rc1{1};
        int rc2{1};
    };
    auto shared = std::make_shared<Shared>();
    bool requests_started = false;
#
    asio::co_spawn(
        ex,
        [&]() -> asio::awaitable<void> {
            auto ec = co_await server.async_open_passive(std::move(server_conn));
            if (ec) {
                server_rc = 10;
                std::cerr << "[HSMS][pipe][reorder] server SELECT 失败: "
                          << ec.message() << "\n";
                server.stop();
                server_done.set();
                co_return;
            }
#
            auto [rec1, req1] = co_await server.async_receive_data(2s);
            if (rec1) {
                server_rc = 11;
                std::cerr << "[HSMS][pipe][reorder] recv#1 失败: "
                          << rec1.message() << "\n";
                server.stop();
                server_done.set();
                co_return;
            }
            auto [rec2, req2] = co_await server.async_receive_data(2s);
            if (rec2) {
                server_rc = 12;
                std::cerr << "[HSMS][pipe][reorder] recv#2 失败: "
                          << rec2.message() << "\n";
                server.stop();
                server_done.set();
                co_return;
            }
#
            // 故意乱序：先回第二个，再回第一个
            const auto rsp2 =
                secs::hsms::make_data_message(server_opts.session_id,
                                              /*stream=*/1,
                                              /*function=*/14,
                                              /*w_bit=*/false,
                                              req2.header.system_bytes,
                                              bytes_view{req2.body.data(),
                                                         req2.body.size()});
            ec = co_await server.async_send(rsp2);
            if (ec) {
                server_rc = 13;
                std::cerr << "[HSMS][pipe][reorder] send#2 失败: "
                          << ec.message() << "\n";
                server.stop();
                server_done.set();
                co_return;
            }
            const auto rsp1 =
                secs::hsms::make_data_message(server_opts.session_id,
                                              /*stream=*/1,
                                              /*function=*/14,
                                              /*w_bit=*/false,
                                              req1.header.system_bytes,
                                              bytes_view{req1.body.data(),
                                                         req1.body.size()});
            ec = co_await server.async_send(rsp1);
            if (ec) {
                server_rc = 14;
                std::cerr << "[HSMS][pipe][reorder] send#1 失败: "
                          << ec.message() << "\n";
                server.stop();
                server_done.set();
                co_return;
            }
#
            server.stop();
            server_rc = 0;
            server_done.set();
            co_return;
        },
        asio::detached);
#
    auto ec = co_await client.async_open_active(std::move(client_conn));
    if (ec) {
        std::cerr << "[HSMS][pipe][reorder] client SELECT 失败: " << ec.message()
                  << "\n";
        rc = 4;
    }
#
    if (rc == 0) {
        asio::co_spawn(
            ex,
            [&, shared]() -> asio::awaitable<void> {
                const auto item = make_test_item(/*tag=*/111U);
                auto enc = encode_item(item);
                if (enc.first) {
                    shared->rc1 = 20;
                    shared->done1.set();
                    co_return;
                }
                auto req = co_await client.async_request_data(
                    1,
                    13,
                    bytes_view{enc.second.data(), enc.second.size()},
                    800ms);
                if (req.first) {
                    shared->rc1 = 21;
                    shared->done1.set();
                    co_return;
                }
                auto dec = decode_item(
                    bytes_view{req.second.body.data(), req.second.body.size()});
                if (dec.first || dec.second != item) {
                    shared->rc1 = 22;
                    shared->done1.set();
                    co_return;
                }
                shared->rc1 = 0;
                shared->done1.set();
                co_return;
            },
            asio::detached);
#
        asio::co_spawn(
            ex,
            [&, shared]() -> asio::awaitable<void> {
                const auto item = make_test_item(/*tag=*/222U);
                auto enc = encode_item(item);
                if (enc.first) {
                    shared->rc2 = 30;
                    shared->done2.set();
                    co_return;
                }
                auto req = co_await client.async_request_data(
                    1,
                    13,
                    bytes_view{enc.second.data(), enc.second.size()},
                    800ms);
                if (req.first) {
                    shared->rc2 = 31;
                    shared->done2.set();
                    co_return;
                }
                auto dec = decode_item(
                    bytes_view{req.second.body.data(), req.second.body.size()});
                if (dec.first || dec.second != item) {
                    shared->rc2 = 32;
                    shared->done2.set();
                    co_return;
                }
                shared->rc2 = 0;
                shared->done2.set();
                co_return;
            },
            asio::detached);
#
        requests_started = true;
#
        ec = co_await shared->done1.async_wait(2s);
        if (ec) {
            std::cerr << "[HSMS][pipe][reorder] 等待 done1 失败: " << ec.message()
                      << "\n";
            rc = 5;
        }
        if (rc == 0) {
            ec = co_await shared->done2.async_wait(2s);
            if (ec) {
                std::cerr << "[HSMS][pipe][reorder] 等待 done2 失败: " << ec.message()
                          << "\n";
                rc = 6;
            }
        }
        if (rc == 0 && shared->rc1 != 0) {
            rc = shared->rc1;
        }
        if (rc == 0 && shared->rc2 != 0) {
            rc = shared->rc2;
        }
    }
#
    client.stop();
    if (requests_started) {
        (void)co_await shared->done1.async_wait(1s);
        (void)co_await shared->done2.async_wait(1s);
    }
    (void)co_await client.async_wait_reader_stopped(1s);
    server.stop();
#
    ec = co_await server_done.async_wait(2s);
    if (ec) {
        std::cerr << "[HSMS][pipe][reorder] 等待 server_done 失败: "
                  << ec.message() << "\n";
        if (rc == 0) {
            rc = 7;
        }
    }
    (void)co_await server.async_wait_reader_stopped(1s);
#
    if (rc == 0 && server_rc != 0) {
        rc = server_rc;
    }
#
    co_return rc;
}
#
static asio::awaitable<int> case_t3_timeout_and_late_response() {
    auto ex = co_await asio::this_coro::executor;
    constexpr std::uint16_t device_id = 1;
#
    const auto server_opts = make_default_opts(device_id);
    auto client_opts = make_default_opts(device_id);
    client_opts.t3 = 150ms;
#
    secs::hsms::Session server(ex, server_opts);
    secs::hsms::Session client(ex, client_opts);
#
    auto [duplex_ec, duplex] =
        make_pipe_duplex(client.executor(), server.executor());
    if (duplex_ec) {
        std::cerr << "[HSMS][pipe][t3] 创建 pipe 失败: " << duplex_ec.message()
                  << "\n";
        co_return 2;
    }
#
    secs::hsms::Connection client_conn(
        std::move(duplex.client_stream),
        secs::hsms::ConnectionOptions{.t8 = client_opts.t8});
    secs::hsms::Connection server_conn(
        std::move(duplex.server_stream),
        secs::hsms::ConnectionOptions{.t8 = server_opts.t8});
#
    secs::core::Event server_done{};
    int server_rc = 1;
    int rc = 0;
#
    asio::co_spawn(
        ex,
        [&]() -> asio::awaitable<void> {
            auto ec = co_await server.async_open_passive(std::move(server_conn));
            if (ec) {
                server_rc = 10;
                std::cerr << "[HSMS][pipe][t3] server SELECT 失败: "
                          << ec.message() << "\n";
                server.stop();
                server_done.set();
                co_return;
            }
#
            auto [rec, req] = co_await server.async_receive_data(2s);
            if (rec) {
                server_rc = 11;
                std::cerr << "[HSMS][pipe][t3] recv 失败: " << rec.message()
                          << "\n";
                server.stop();
                server_done.set();
                co_return;
            }
#
            // 故意延迟超过 client T3，验证 client 事务超时不崩溃。
            asio::steady_timer t(ex);
            t.expires_after(400ms);
            (void)co_await t.async_wait(asio::as_tuple(asio::use_awaitable));
#
            const auto rsp =
                secs::hsms::make_data_message(server_opts.session_id,
                                              /*stream=*/1,
                                              /*function=*/14,
                                              /*w_bit=*/false,
                                              req.header.system_bytes,
                                              bytes_view{req.body.data(),
                                                         req.body.size()});
            ec = co_await server.async_send(rsp);
            if (ec) {
                server_rc = 12;
                std::cerr << "[HSMS][pipe][t3] send 失败: " << ec.message()
                          << "\n";
                server.stop();
                server_done.set();
                co_return;
            }
#
            // 给对端一点时间把 late response 收到 inbound 队列
            t.expires_after(50ms);
            (void)co_await t.async_wait(asio::as_tuple(asio::use_awaitable));
#
            server.stop();
            server_rc = 0;
            server_done.set();
            co_return;
        },
        asio::detached);
#
    auto ec = co_await client.async_open_active(std::move(client_conn));
    if (ec) {
        std::cerr << "[HSMS][pipe][t3] client SELECT 失败: " << ec.message()
                  << "\n";
        rc = 4;
    }
#
    if (rc == 0) {
        const auto item = make_test_item(/*tag=*/333U);
        auto enc = encode_item(item);
        if (enc.first) {
            std::cerr << "[HSMS][pipe][t3] client 编码失败: " << enc.first.message()
                      << "\n";
            rc = 5;
        } else {
            auto req = co_await client.async_request_data(
                1,
                13,
                bytes_view{enc.second.data(), enc.second.size()},
                /*timeout=*/client_opts.t3);
            if (req.first != make_error_code(errc::timeout)) {
                std::cerr << "[HSMS][pipe][t3] 期望 timeout，实际: ["
                          << req.first.value() << "] " << req.first.message()
                          << "\n";
                rc = 6;
            } else {
                // late response 应进入 inbound_data_，可被 async_receive_data 拿到。
                auto late = co_await client.async_receive_data(1s);
                if (late.first) {
                    std::cerr << "[HSMS][pipe][t3] 未收到 late response: "
                              << late.first.message() << "\n";
                    rc = 7;
                } else {
                    auto dec = decode_item(bytes_view{late.second.body.data(),
                                                      late.second.body.size()});
                    if (dec.first || dec.second != item) {
                        std::cerr << "[HSMS][pipe][t3] late response 解码或内容不符\n";
                        rc = 8;
                    }
                }
            }
        }
    }
#
    client.stop();
    (void)co_await client.async_wait_reader_stopped(1s);
    server.stop();
#
    ec = co_await server_done.async_wait(2s);
    if (ec) {
        std::cerr << "[HSMS][pipe][t3] 等待 server_done 失败: " << ec.message()
                  << "\n";
        rc = (rc == 0) ? 9 : rc;
    }
    (void)co_await server.async_wait_reader_stopped(1s);
#
    if (rc == 0 && server_rc != 0) {
        rc = server_rc;
    }
    co_return rc;
}
#
static asio::awaitable<int> case_separate_disconnect() {
    auto ex = co_await asio::this_coro::executor;
    constexpr std::uint16_t device_id = 1;
#
    const auto server_opts = make_default_opts(device_id);
    auto client_opts = make_default_opts(device_id);
#
    secs::hsms::Session server(ex, server_opts);
    secs::hsms::Session client(ex, client_opts);
#
    auto [duplex_ec, duplex] =
        make_pipe_duplex(client.executor(), server.executor());
    if (duplex_ec) {
        std::cerr << "[HSMS][pipe][separate] 创建 pipe 失败: " << duplex_ec.message()
                  << "\n";
        co_return 2;
    }
#
    secs::hsms::Connection client_conn(
        std::move(duplex.client_stream),
        secs::hsms::ConnectionOptions{.t8 = client_opts.t8});
    secs::hsms::Connection server_conn(
        std::move(duplex.server_stream),
        secs::hsms::ConnectionOptions{.t8 = server_opts.t8});
#
    secs::core::Event server_done{};
    int server_rc = 1;
    int rc = 0;
#
    asio::co_spawn(
        ex,
        [&]() -> asio::awaitable<void> {
            auto ec = co_await server.async_open_passive(std::move(server_conn));
            if (ec) {
                server_rc = 10;
                std::cerr << "[HSMS][pipe][separate] server SELECT 失败: "
                          << ec.message() << "\n";
                server.stop();
                server_done.set();
                co_return;
            }
#
            // 主动发送 SEPARATE.req，期望对端断线收敛（cancelled）。
            const auto sep = secs::hsms::make_separate_req(
                /*session_id=*/0xFFFF,
                server.allocate_system_bytes());
            ec = co_await server.async_send(sep);
            if (ec) {
                server_rc = 11;
                std::cerr << "[HSMS][pipe][separate] server send SEPARATE 失败: "
                          << ec.message() << "\n";
                server.stop();
                server_done.set();
                co_return;
            }
#
            // 等待 client 关闭后，本端 reader 也会因读错误退出。
            (void)co_await server.async_wait_reader_stopped(2s);
            server.stop();
            server_rc = 0;
            server_done.set();
            co_return;
        },
        asio::detached);
#
    auto ec = co_await client.async_open_active(std::move(client_conn));
    if (ec) {
        std::cerr << "[HSMS][pipe][separate] client SELECT 失败: " << ec.message()
                  << "\n";
        rc = 20;
    } else {
        // 等待对端 SEPARATE.req 触发断线
        (void)co_await client.async_wait_reader_stopped(2s);
        auto [rec, _] = co_await client.async_receive_data(std::nullopt);
        if (rec != make_error_code(errc::cancelled)) {
            std::cerr << "[HSMS][pipe][separate] 期望 cancelled，实际: ["
                      << rec.value() << "] " << rec.message() << "\n";
            rc = 21;
        }
    }
#
    client.stop();
    (void)co_await client.async_wait_reader_stopped(1s);
    server.stop();
#
    ec = co_await server_done.async_wait(2s);
    if (ec) {
        std::cerr << "[HSMS][pipe][separate] 等待 server_done 失败: " << ec.message()
                  << "\n";
        rc = (rc == 0) ? 22 : rc;
    }
#
    if (rc == 0 && server_rc != 0) {
        rc = server_rc;
    }
    co_return rc;
}
#
static asio::awaitable<int> case_inbound_overflow_disconnect() {
    auto ex = co_await asio::this_coro::executor;
    constexpr std::uint16_t device_id = 1;
#
    auto server_opts = make_default_opts(device_id);
    auto client_opts = make_default_opts(device_id);
    client_opts.max_inbound_data_messages = 2;
#
    secs::hsms::Session server(ex, server_opts);
    secs::hsms::Session client(ex, client_opts);
#
    auto [duplex_ec, duplex] =
        make_pipe_duplex(client.executor(), server.executor());
    if (duplex_ec) {
        std::cerr << "[HSMS][pipe][inbound_overflow] 创建 pipe 失败: "
                  << duplex_ec.message() << "\n";
        co_return 2;
    }
#
    secs::hsms::Connection client_conn(
        std::move(duplex.client_stream),
        secs::hsms::ConnectionOptions{.t8 = client_opts.t8});
    secs::hsms::Connection server_conn(
        std::move(duplex.server_stream),
        secs::hsms::ConnectionOptions{.t8 = server_opts.t8});
#
    secs::core::Event server_done{};
    int server_rc = 1;
    int rc = 0;
#
    asio::co_spawn(
        ex,
        [&]() -> asio::awaitable<void> {
            auto ec = co_await server.async_open_passive(std::move(server_conn));
            if (ec) {
                server_rc = 10;
                std::cerr << "[HSMS][pipe][inbound_overflow] server SELECT 失败: "
                          << ec.message() << "\n";
                server.stop();
                server_done.set();
                co_return;
            }
#
            // 连续发送 data（对端不读取），触发对端 inbound 队列溢出断线。
            std::vector<byte> body(8, static_cast<byte>(0xAB));
            for (int i = 0; i < 10; ++i) {
                const auto msg = secs::hsms::make_data_message(
                    server_opts.session_id,
                    /*stream=*/1,
                    /*function=*/1,
                    /*w_bit=*/false,
                    server.allocate_system_bytes(),
                    bytes_view{body.data(), body.size()});
                ec = co_await server.async_send(msg);
                if (ec) {
                    break;
                }
            }
#
            (void)co_await server.async_wait_reader_stopped(2s);
            server.stop();
            server_rc = 0;
            server_done.set();
            co_return;
        },
        asio::detached);
#
    auto ec = co_await client.async_open_active(std::move(client_conn));
    if (ec) {
        std::cerr << "[HSMS][pipe][inbound_overflow] client SELECT 失败: "
                  << ec.message() << "\n";
        rc = 20;
    } else {
        // 等待 client 读协程退出（预计由 inbound 溢出触发）。
        ec = co_await client.async_wait_reader_stopped(2s);
        if (ec) {
            std::cerr << "[HSMS][pipe][inbound_overflow] 等待 reader_stopped 失败: "
                      << ec.message() << "\n";
            rc = 21;
        } else {
            auto [rec, _] = co_await client.async_receive_data(std::nullopt);
            if (rec != make_error_code(errc::buffer_overflow)) {
                std::cerr << "[HSMS][pipe][inbound_overflow] 期望 buffer_overflow，实际: ["
                          << rec.value() << "] " << rec.message() << "\n";
                rc = 22;
            }
        }
    }
#
    client.stop();
    (void)co_await client.async_wait_reader_stopped(1s);
    server.stop();
#
    ec = co_await server_done.async_wait(2s);
    if (ec) {
        std::cerr << "[HSMS][pipe][inbound_overflow] 等待 server_done 失败: "
                  << ec.message() << "\n";
        rc = (rc == 0) ? 23 : rc;
    }
#
    if (rc == 0 && server_rc != 0) {
        rc = server_rc;
    }
    co_return rc;
}
#
static asio::awaitable<int> case_pending_overflow_is_nonfatal() {
    auto ex = co_await asio::this_coro::executor;
    constexpr std::uint16_t device_id = 1;
#
    const auto server_opts = make_default_opts(device_id);
    auto client_opts = make_default_opts(device_id);
    client_opts.max_pending_requests = 2;
#
    secs::hsms::Session server(ex, server_opts);
    secs::hsms::Session client(ex, client_opts);
#
    auto [duplex_ec, duplex] =
        make_pipe_duplex(client.executor(), server.executor());
    if (duplex_ec) {
        std::cerr << "[HSMS][pipe][pending_overflow] 创建 pipe 失败: "
                  << duplex_ec.message() << "\n";
        co_return 2;
    }
#
    secs::hsms::Connection client_conn(
        std::move(duplex.client_stream),
        secs::hsms::ConnectionOptions{.t8 = client_opts.t8});
    secs::hsms::Connection server_conn(
        std::move(duplex.server_stream),
        secs::hsms::ConnectionOptions{.t8 = server_opts.t8});
#
    secs::core::Event two_received{};
    secs::core::Event allow_response{};
    secs::core::Event client_done{};
    secs::core::Event server_done{};
    int server_rc = 1;
    int rc = 0;
#
    asio::co_spawn(
        ex,
        [&]() -> asio::awaitable<void> {
            auto ec = co_await server.async_open_passive(std::move(server_conn));
            if (ec) {
                server_rc = 10;
                std::cerr << "[HSMS][pipe][pending_overflow] server SELECT 失败: "
                          << ec.message() << "\n";
                server.stop();
                server_done.set();
                co_return;
            }
#
            // 收两条请求但先不回，制造 pending_ 堆积。
            auto [rec1, req1] = co_await server.async_receive_data(2s);
            if (rec1) {
                server_rc = 11;
                std::cerr << "[HSMS][pipe][pending_overflow] recv#1 失败: "
                          << rec1.message() << "\n";
                server.stop();
                server_done.set();
                co_return;
            }
            auto [rec2, req2] = co_await server.async_receive_data(2s);
            if (rec2) {
                server_rc = 12;
                std::cerr << "[HSMS][pipe][pending_overflow] recv#2 失败: "
                          << rec2.message() << "\n";
                server.stop();
                server_done.set();
                co_return;
            }
            two_received.set();
#
            (void)co_await allow_response.async_wait(2s);
#
            const auto rsp1 = secs::hsms::make_data_message(
                server_opts.session_id,
                /*stream=*/1,
                /*function=*/14,
                /*w_bit=*/false,
                req1.header.system_bytes,
                bytes_view{req1.body.data(), req1.body.size()});
            const auto rsp2 = secs::hsms::make_data_message(
                server_opts.session_id,
                /*stream=*/1,
                /*function=*/14,
                /*w_bit=*/false,
                req2.header.system_bytes,
                bytes_view{req2.body.data(), req2.body.size()});
#
            ec = co_await server.async_send(rsp1);
            if (ec) {
                server_rc = 13;
                std::cerr << "[HSMS][pipe][pending_overflow] send#1 失败: "
                          << ec.message() << "\n";
            }
            ec = co_await server.async_send(rsp2);
            if (ec) {
                server_rc = 14;
                std::cerr << "[HSMS][pipe][pending_overflow] send#2 失败: "
                          << ec.message() << "\n";
            }
#
            // 等待 client 完成后续验证（例如 LINKTEST），再退出。
            (void)co_await client_done.async_wait(3s);
            server.stop();
            server_rc = 0;
            server_done.set();
            co_return;
        },
        asio::detached);
#
    auto ec = co_await client.async_open_active(std::move(client_conn));
    if (ec) {
        std::cerr << "[HSMS][pipe][pending_overflow] client SELECT 失败: "
                  << ec.message() << "\n";
        rc = 20;
    } else {
        // 先并发起两条请求，等待 server 确认已收到（pending 堆积）。
        struct TwoReq final {
            secs::core::Event done1{};
            secs::core::Event done2{};
            int rc1{1};
            int rc2{1};
        };
        auto two = std::make_shared<TwoReq>();
#
        asio::co_spawn(
            ex,
            [&client, two]() -> asio::awaitable<void> {
                const auto item = make_test_item(/*tag=*/1001U);
                auto enc = encode_item(item);
                if (enc.first) {
                    two->rc1 = 30;
                    two->done1.set();
                    co_return;
                }
                auto req = co_await client.async_request_data(
                    1, 13, bytes_view{enc.second.data(), enc.second.size()}, 2s);
                if (req.first) {
                    two->rc1 = 31;
                    two->done1.set();
                    co_return;
                }
                auto dec =
                    decode_item(bytes_view{req.second.body.data(),
                                           req.second.body.size()});
                if (dec.first || dec.second != item) {
                    two->rc1 = 32;
                    two->done1.set();
                    co_return;
                }
                two->rc1 = 0;
                two->done1.set();
                co_return;
            },
            asio::detached);
#
        asio::co_spawn(
            ex,
            [&client, two]() -> asio::awaitable<void> {
                const auto item = make_test_item(/*tag=*/1002U);
                auto enc = encode_item(item);
                if (enc.first) {
                    two->rc2 = 40;
                    two->done2.set();
                    co_return;
                }
                auto req = co_await client.async_request_data(
                    1, 13, bytes_view{enc.second.data(), enc.second.size()}, 2s);
                if (req.first) {
                    two->rc2 = 41;
                    two->done2.set();
                    co_return;
                }
                auto dec =
                    decode_item(bytes_view{req.second.body.data(),
                                           req.second.body.size()});
                if (dec.first || dec.second != item) {
                    two->rc2 = 42;
                    two->done2.set();
                    co_return;
                }
                two->rc2 = 0;
                two->done2.set();
                co_return;
            },
            asio::detached);
#
        ec = co_await two_received.async_wait(2s);
        if (ec) {
            std::cerr << "[HSMS][pipe][pending_overflow] 等待 two_received 失败: "
                      << ec.message() << "\n";
            rc = 21;
        } else {
            // 第三条请求应立即因 pending 上限返回 buffer_overflow，且不影响会话继续工作。
            const auto item = make_test_item(/*tag=*/1003U);
            auto enc = encode_item(item);
            if (enc.first) {
                rc = 22;
            } else {
                auto req = co_await client.async_request_data(
                    1, 13, bytes_view{enc.second.data(), enc.second.size()}, 2s);
                if (req.first != make_error_code(errc::buffer_overflow)) {
                    std::cerr << "[HSMS][pipe][pending_overflow] 期望 buffer_overflow，实际: ["
                              << req.first.value() << "] "
                              << req.first.message() << "\n";
                    rc = 23;
                }
            }
        }
#
        allow_response.set();
#
        if (rc == 0) {
            (void)co_await two->done1.async_wait(2s);
            (void)co_await two->done2.async_wait(2s);
            if (two->rc1 != 0) {
                rc = two->rc1;
            } else if (two->rc2 != 0) {
                rc = two->rc2;
            } else {
                // 通过 LINKTEST 验证会话仍可用（pending 溢出不应导致断线）。
                auto lec = co_await client.async_linktest();
                if (lec) {
                    std::cerr
                        << "[HSMS][pipe][pending_overflow] 后续 LINKTEST 失败: "
                        << lec.message() << "\n";
                    rc = 24;
                }
            }
        }
    }
#
    client_done.set();
#
    client.stop();
    (void)co_await client.async_wait_reader_stopped(1s);
    server.stop();
#
    ec = co_await server_done.async_wait(2s);
    if (ec) {
        std::cerr << "[HSMS][pipe][pending_overflow] 等待 server_done 失败: "
                  << ec.message() << "\n";
        rc = (rc == 0) ? 27 : rc;
    }
#
    if (rc == 0 && server_rc != 0) {
        rc = server_rc;
    }
    co_return rc;
}
#
static asio::awaitable<int> case_stop_during_pending_request() {
    auto ex = co_await asio::this_coro::executor;
    constexpr std::uint16_t device_id = 1;
#
    const auto server_opts = make_default_opts(device_id);
    auto client_opts = make_default_opts(device_id);
    client_opts.t3 = 2s;
#
    secs::hsms::Session server(ex, server_opts);
    secs::hsms::Session client(ex, client_opts);
#
    auto [duplex_ec, duplex] =
        make_pipe_duplex(client.executor(), server.executor());
    if (duplex_ec) {
        std::cerr << "[HSMS][pipe][stop_pending] 创建 pipe 失败: "
                  << duplex_ec.message() << "\n";
        co_return 2;
    }
#
    secs::hsms::Connection client_conn(
        std::move(duplex.client_stream),
        secs::hsms::ConnectionOptions{.t8 = client_opts.t8});
    secs::hsms::Connection server_conn(
        std::move(duplex.server_stream),
        secs::hsms::ConnectionOptions{.t8 = server_opts.t8});
#
    secs::core::Event got_req{};
    secs::core::Event client_done{};
    secs::core::Event server_done{};
    int server_rc = 1;
    int rc = 0;
#
    asio::co_spawn(
        ex,
        [&]() -> asio::awaitable<void> {
            auto ec = co_await server.async_open_passive(std::move(server_conn));
            if (ec) {
                server_rc = 10;
                std::cerr << "[HSMS][pipe][stop_pending] server SELECT 失败: "
                          << ec.message() << "\n";
                server.stop();
                server_done.set();
                co_return;
            }
#
            auto [rec, _] = co_await server.async_receive_data(2s);
            if (rec) {
                server_rc = 11;
                std::cerr << "[HSMS][pipe][stop_pending] server recv 失败: "
                          << rec.message() << "\n";
                server.stop();
                server_done.set();
                co_return;
            }
            got_req.set();
#
            (void)co_await client_done.async_wait(3s);
            server.stop();
            (void)co_await server.async_wait_reader_stopped(1s);
            server_rc = 0;
            server_done.set();
            co_return;
        },
        asio::detached);
#
    auto ec = co_await client.async_open_active(std::move(client_conn));
    if (ec) {
        std::cerr << "[HSMS][pipe][stop_pending] client SELECT 失败: "
                  << ec.message() << "\n";
        rc = 20;
    } else {
        struct Result final {
            secs::core::Event done{};
            std::error_code ec{};
        };
        auto result = std::make_shared<Result>();
#
        asio::co_spawn(
            ex,
            [&client, result]() -> asio::awaitable<void> {
                const auto item = make_test_item(/*tag=*/2001U);
                auto enc = encode_item(item);
                if (enc.first) {
                    result->ec = enc.first;
                    result->done.set();
                    co_return;
                }
                auto req = co_await client.async_request_data(
                    1,
                    13,
                    bytes_view{enc.second.data(), enc.second.size()},
                    2s);
                result->ec = req.first;
                result->done.set();
                co_return;
            },
            asio::detached);
#
        ec = co_await got_req.async_wait(2s);
        if (ec) {
            std::cerr << "[HSMS][pipe][stop_pending] 等待 got_req 失败: "
                      << ec.message() << "\n";
            rc = 21;
        } else {
            // 在请求仍挂起时 stop()，期望 pending 以 cancelled 收敛。
            client.stop();
            (void)co_await client.async_wait_reader_stopped(1s);
#
            ec = co_await result->done.async_wait(2s);
            if (ec) {
                std::cerr << "[HSMS][pipe][stop_pending] 等待 request 完成失败: "
                          << ec.message() << "\n";
                rc = 22;
            } else if (result->ec != make_error_code(errc::cancelled)) {
                std::cerr << "[HSMS][pipe][stop_pending] 期望 cancelled，实际: ["
                          << result->ec.value() << "] " << result->ec.message()
                          << "\n";
                rc = 23;
            }
        }
    }
#
    client_done.set();
#
    client.stop();
    (void)co_await client.async_wait_reader_stopped(1s);
    server.stop();
#
    ec = co_await server_done.async_wait(2s);
    if (ec) {
        std::cerr << "[HSMS][pipe][stop_pending] 等待 server_done 失败: "
                  << ec.message() << "\n";
        rc = (rc == 0) ? 24 : rc;
    }
#
    if (rc == 0 && server_rc != 0) {
        rc = server_rc;
    }
    co_return rc;
}
#
static asio::awaitable<int> run_all() {
    if (auto rc = co_await case_basic_request_response(); rc != 0) {
        co_return rc;
    }
    if (auto rc = co_await case_out_of_order_responses(); rc != 0) {
        co_return rc;
    }
    if (auto rc = co_await case_t3_timeout_and_late_response(); rc != 0) {
        co_return rc;
    }
    if (auto rc = co_await case_separate_disconnect(); rc != 0) {
        co_return rc;
    }
    if (auto rc = co_await case_inbound_overflow_disconnect(); rc != 0) {
        co_return rc;
    }
    if (auto rc = co_await case_pending_overflow_is_nonfatal(); rc != 0) {
        co_return rc;
    }
    if (auto rc = co_await case_stop_during_pending_request(); rc != 0) {
        co_return rc;
    }
    co_return 0;
}
#
} // namespace
#
int main() {
    // 避免写入断管触发 SIGPIPE 终止进程（pipe 用例必需）。
    ::signal(SIGPIPE, SIG_IGN);
#
    asio::io_context io;
    int rc = 1;
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            rc = co_await run_all();
            io.stop();
        },
        asio::detached);
    io.run();
    return rc;
}
