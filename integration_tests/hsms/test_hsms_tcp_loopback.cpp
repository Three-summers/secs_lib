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
#include <asio/ip/tcp.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#
#include <signal.h>
#
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
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
static bool is_socket_not_permitted(const std::error_code &ec) noexcept {
    // 兼容 std::generic_category 与 Asio 的 system_category。
    return ec.value() == EPERM || ec.value() == EACCES;
}
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
static std::pair<std::error_code, asio::ip::tcp::acceptor>
make_loopback_acceptor(asio::any_io_executor ex) {
    asio::ip::tcp::acceptor acceptor(ex);
    std::error_code ec;
#
    acceptor.open(asio::ip::tcp::v4(), ec);
    if (ec) {
        return {ec, std::move(acceptor)};
    }
#
    acceptor.set_option(asio::ip::tcp::acceptor::reuse_address(true), ec);
    if (ec) {
        return {ec, std::move(acceptor)};
    }
#
    acceptor.bind(
        asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0), ec);
    if (ec) {
        return {ec, std::move(acceptor)};
    }
#
    acceptor.listen(asio::socket_base::max_listen_connections, ec);
    if (ec) {
        return {ec, std::move(acceptor)};
    }
#
    return {std::error_code{}, std::move(acceptor)};
}
#
static asio::awaitable<std::pair<std::error_code, asio::ip::tcp::socket>>
async_accept_one(asio::ip::tcp::acceptor &acceptor) {
    auto [ec, socket] =
        co_await acceptor.async_accept(asio::as_tuple(asio::use_awaitable));
    if (ec) {
        co_return std::pair{ec, asio::ip::tcp::socket(acceptor.get_executor())};
    }
    co_return std::pair{std::error_code{}, std::move(socket)};
}
#
static asio::awaitable<int> case_basic_request_response() {
    auto ex = co_await asio::this_coro::executor;
    constexpr std::uint16_t device_id = 1;
#
    auto [acc_ec, acceptor] = make_loopback_acceptor(ex);
    if (acc_ec) {
        if (is_socket_not_permitted(acc_ec)) {
            std::cerr << "[HSMS][tcp][loopback][basic] socket 被禁用，跳过\n";
            co_return 77;
        }
        std::cerr << "[HSMS][tcp][loopback][basic] listen 失败: "
                  << acc_ec.message() << "\n";
        co_return 2;
    }
#
    std::error_code ep_ec;
    const auto ep = acceptor.local_endpoint(ep_ec);
    if (ep_ec) {
        std::cerr << "[HSMS][tcp][loopback][basic] local_endpoint 失败: "
                  << ep_ec.message() << "\n";
        co_return 3;
    }
#
    const auto server_opts = make_default_opts(device_id);
    auto client_opts = make_default_opts(device_id);
#
    struct Shared final {
        secs::core::Event server_done{};
        secs::core::Event client_done{};
        int server_rc{1};
        int client_rc{1};
    };
    auto shared = std::make_shared<Shared>();
#
    asio::co_spawn(
        ex,
        [acc = std::move(acceptor), shared, server_opts]() mutable
        -> asio::awaitable<void> {
            auto [accept_ec, socket] = co_await async_accept_one(acc);
            if (accept_ec) {
                shared->server_rc = 10;
                std::cerr << "[HSMS][tcp][loopback][basic] accept 失败: "
                          << accept_ec.message() << "\n";
                shared->server_done.set();
                co_return;
            }
#
            secs::hsms::Session server(acc.get_executor(), server_opts);
            auto ec = co_await server.async_open_passive(std::move(socket));
            if (ec) {
                shared->server_rc = 11;
                std::cerr << "[HSMS][tcp][loopback][basic] server SELECT 失败: "
                          << ec.message() << "\n";
                server.stop();
                (void)co_await server.async_wait_reader_stopped(2s);
                shared->server_done.set();
                co_return;
            }
#
            // 收到 S1F13 -> 回 S1F14（echo body）
            auto [rec, req] = co_await server.async_receive_data(2s);
            if (rec) {
                shared->server_rc = 12;
                std::cerr << "[HSMS][tcp][loopback][basic] server recv 失败: "
                          << rec.message() << "\n";
                server.stop();
                (void)co_await server.async_wait_reader_stopped(2s);
                shared->server_done.set();
                co_return;
            }
#
            auto [dec_ec, _] = decode_item(
                bytes_view{req.body.data(), req.body.size()});
            if (dec_ec) {
                shared->server_rc = 13;
                std::cerr << "[HSMS][tcp][loopback][basic] server 解码失败: "
                          << dec_ec.message() << "\n";
                server.stop();
                (void)co_await server.async_wait_reader_stopped(2s);
                shared->server_done.set();
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
                shared->server_rc = 14;
                std::cerr << "[HSMS][tcp][loopback][basic] server send 失败: "
                          << ec.message() << "\n";
                server.stop();
                (void)co_await server.async_wait_reader_stopped(2s);
                shared->server_done.set();
                co_return;
            }
#
            server.stop();
            (void)co_await server.async_wait_reader_stopped(2s);
            shared->server_rc = 0;
            shared->server_done.set();
            co_return;
        },
        asio::detached);
#
    asio::co_spawn(
        ex,
        [shared, client_opts, ep]() -> asio::awaitable<void> {
            secs::hsms::Session client(co_await asio::this_coro::executor,
                                       client_opts);
#
            auto ec = co_await client.async_open_active(ep);
            if (ec) {
                shared->client_rc = 20;
                std::cerr << "[HSMS][tcp][loopback][basic] client SELECT 失败: "
                          << ec.message() << "\n";
                client.stop();
                (void)co_await client.async_wait_reader_stopped(2s);
                shared->client_done.set();
                co_return;
            }
#
            ec = co_await client.async_linktest();
            if (ec) {
                shared->client_rc = 21;
                std::cerr << "[HSMS][tcp][loopback][basic] client LINKTEST 失败: "
                          << ec.message() << "\n";
                client.stop();
                (void)co_await client.async_wait_reader_stopped(2s);
                shared->client_done.set();
                co_return;
            }
#
            const auto item = make_test_item(/*tag=*/123U);
            auto [enc_ec, body] = encode_item(item);
            if (enc_ec) {
                shared->client_rc = 22;
                std::cerr << "[HSMS][tcp][loopback][basic] client 编码失败: "
                          << enc_ec.message() << "\n";
                client.stop();
                (void)co_await client.async_wait_reader_stopped(2s);
                shared->client_done.set();
                co_return;
            }
#
            auto [rec, rsp] =
                co_await client.async_request_data(1,
                                                   13,
                                                   bytes_view{body.data(),
                                                              body.size()});
            if (rec) {
                shared->client_rc = 23;
                std::cerr << "[HSMS][tcp][loopback][basic] client request 失败: "
                          << rec.message() << "\n";
                client.stop();
                (void)co_await client.async_wait_reader_stopped(2s);
                shared->client_done.set();
                co_return;
            }
#
            auto [dec_ec, out_item] =
                decode_item(bytes_view{rsp.body.data(), rsp.body.size()});
            if (dec_ec) {
                shared->client_rc = 24;
                std::cerr << "[HSMS][tcp][loopback][basic] client 解码失败: "
                          << dec_ec.message() << "\n";
                client.stop();
                (void)co_await client.async_wait_reader_stopped(2s);
                shared->client_done.set();
                co_return;
            }
#
            // 最小一致性验证：回包能解码，且 tag 保持一致。
            const auto *items = out_item.get_if<secs::ii::List>();
            if (!items || items->empty()) {
                shared->client_rc = 25;
                std::cerr << "[HSMS][tcp][loopback][basic] 回包结构不符\n";
                client.stop();
                (void)co_await client.async_wait_reader_stopped(2s);
                shared->client_done.set();
                co_return;
            }
            const auto *tag_item = (*items)[0].get_if<secs::ii::U4>();
            if (!tag_item || tag_item->values.empty() ||
                tag_item->values[0] != 123U) {
                shared->client_rc = 26;
                std::cerr << "[HSMS][tcp][loopback][basic] 回包 tag 不符\n";
                client.stop();
                (void)co_await client.async_wait_reader_stopped(2s);
                shared->client_done.set();
                co_return;
            }
#
            client.stop();
            (void)co_await client.async_wait_reader_stopped(2s);
            shared->client_rc = 0;
            shared->client_done.set();
            co_return;
        },
        asio::detached);
#
    // join（避免 detached 协程泄漏到下一用例）
    auto ec = co_await shared->server_done.async_wait(3s);
    if (ec) {
        std::cerr << "[HSMS][tcp][loopback][basic] 等待 server_done 失败: "
                  << ec.message() << "\n";
        co_return 4;
    }
    ec = co_await shared->client_done.async_wait(3s);
    if (ec) {
        std::cerr << "[HSMS][tcp][loopback][basic] 等待 client_done 失败: "
                  << ec.message() << "\n";
        co_return 5;
    }
#
    if (shared->server_rc != 0) {
        co_return shared->server_rc;
    }
    if (shared->client_rc != 0) {
        co_return shared->client_rc;
    }
#
    co_return 0;
}
#
static asio::awaitable<int> case_out_of_order_responses() {
    auto ex = co_await asio::this_coro::executor;
    constexpr std::uint16_t device_id = 1;
#
    auto [acc_ec, acceptor] = make_loopback_acceptor(ex);
    if (acc_ec) {
        if (is_socket_not_permitted(acc_ec)) {
            std::cerr << "[HSMS][tcp][loopback][reorder] socket 被禁用，跳过\n";
            co_return 77;
        }
        std::cerr << "[HSMS][tcp][loopback][reorder] listen 失败: "
                  << acc_ec.message() << "\n";
        co_return 2;
    }
#
    std::error_code ep_ec;
    const auto ep = acceptor.local_endpoint(ep_ec);
    if (ep_ec) {
        std::cerr << "[HSMS][tcp][loopback][reorder] local_endpoint 失败: "
                  << ep_ec.message() << "\n";
        co_return 3;
    }
#
    const auto server_opts = make_default_opts(device_id);
    auto client_opts = make_default_opts(device_id);
#
    struct Shared final {
        secs::core::Event server_done{};
        secs::core::Event client_done{};
        int server_rc{1};
        int client_rc{1};
    };
    auto shared = std::make_shared<Shared>();
#
    asio::co_spawn(
        ex,
        [acc = std::move(acceptor), shared, server_opts]() mutable
        -> asio::awaitable<void> {
            auto [accept_ec, socket] = co_await async_accept_one(acc);
            if (accept_ec) {
                shared->server_rc = 10;
                std::cerr << "[HSMS][tcp][loopback][reorder] accept 失败: "
                          << accept_ec.message() << "\n";
                shared->server_done.set();
                co_return;
            }
#
            secs::hsms::Session server(acc.get_executor(), server_opts);
            auto ec = co_await server.async_open_passive(std::move(socket));
            if (ec) {
                shared->server_rc = 11;
                std::cerr << "[HSMS][tcp][loopback][reorder] server SELECT 失败: "
                          << ec.message() << "\n";
                server.stop();
                (void)co_await server.async_wait_reader_stopped(2s);
                shared->server_done.set();
                co_return;
            }
#
            auto [rec1, req1] = co_await server.async_receive_data(2s);
            if (rec1) {
                shared->server_rc = 12;
                std::cerr << "[HSMS][tcp][loopback][reorder] recv#1 失败: "
                          << rec1.message() << "\n";
                server.stop();
                (void)co_await server.async_wait_reader_stopped(2s);
                shared->server_done.set();
                co_return;
            }
            auto [rec2, req2] = co_await server.async_receive_data(2s);
            if (rec2) {
                shared->server_rc = 13;
                std::cerr << "[HSMS][tcp][loopback][reorder] recv#2 失败: "
                          << rec2.message() << "\n";
                server.stop();
                (void)co_await server.async_wait_reader_stopped(2s);
                shared->server_done.set();
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
                shared->server_rc = 14;
                std::cerr << "[HSMS][tcp][loopback][reorder] send#2 失败: "
                          << ec.message() << "\n";
                server.stop();
                (void)co_await server.async_wait_reader_stopped(2s);
                shared->server_done.set();
                co_return;
            }
#
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
                shared->server_rc = 15;
                std::cerr << "[HSMS][tcp][loopback][reorder] send#1 失败: "
                          << ec.message() << "\n";
                server.stop();
                (void)co_await server.async_wait_reader_stopped(2s);
                shared->server_done.set();
                co_return;
            }
#
            server.stop();
            (void)co_await server.async_wait_reader_stopped(2s);
            shared->server_rc = 0;
            shared->server_done.set();
            co_return;
        },
        asio::detached);
#
    asio::co_spawn(
        ex,
        [shared, client_opts, ep]() -> asio::awaitable<void> {
            auto ex2 = co_await asio::this_coro::executor;
            secs::hsms::Session client(ex2, client_opts);
            bool requests_started = false;
#
            auto ec = co_await client.async_open_active(ep);
            if (ec) {
                shared->client_rc = 20;
                std::cerr << "[HSMS][tcp][loopback][reorder] client SELECT 失败: "
                          << ec.message() << "\n";
                client.stop();
                (void)co_await client.async_wait_reader_stopped(2s);
                shared->client_done.set();
                co_return;
            }
#
            struct TwoReq final {
                secs::core::Event done1{};
                secs::core::Event done2{};
                int rc1{1};
                int rc2{1};
            };
            auto two = std::make_shared<TwoReq>();
#
            asio::co_spawn(
                ex2,
                [&client, two]() -> asio::awaitable<void> {
                    const auto item = make_test_item(/*tag=*/111U);
                    auto enc = encode_item(item);
                    if (enc.first) {
                        two->rc1 = 30;
                        two->done1.set();
                        co_return;
                    }
                    auto req = co_await client.async_request_data(
                        1,
                        13,
                        bytes_view{enc.second.data(), enc.second.size()},
                        800ms);
                    if (req.first) {
                        two->rc1 = 31;
                        two->done1.set();
                        co_return;
                    }
                    auto dec = decode_item(bytes_view{req.second.body.data(),
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
                ex2,
                [&client, two]() -> asio::awaitable<void> {
                    const auto item = make_test_item(/*tag=*/222U);
                    auto enc = encode_item(item);
                    if (enc.first) {
                        two->rc2 = 40;
                        two->done2.set();
                        co_return;
                    }
                    auto req = co_await client.async_request_data(
                        1,
                        13,
                        bytes_view{enc.second.data(), enc.second.size()},
                        800ms);
                    if (req.first) {
                        two->rc2 = 41;
                        two->done2.set();
                        co_return;
                    }
                    auto dec = decode_item(bytes_view{req.second.body.data(),
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
            requests_started = true;
#
            ec = co_await two->done1.async_wait(2s);
            if (ec) {
                shared->client_rc = 21;
                std::cerr << "[HSMS][tcp][loopback][reorder] 等待 done1 失败: "
                          << ec.message() << "\n";
                client.stop();
                if (requests_started) {
                    (void)co_await two->done1.async_wait(1s);
                    (void)co_await two->done2.async_wait(1s);
                }
                (void)co_await client.async_wait_reader_stopped(2s);
                shared->client_done.set();
                co_return;
            }
            ec = co_await two->done2.async_wait(2s);
            if (ec) {
                shared->client_rc = 22;
                std::cerr << "[HSMS][tcp][loopback][reorder] 等待 done2 失败: "
                          << ec.message() << "\n";
                client.stop();
                if (requests_started) {
                    (void)co_await two->done1.async_wait(1s);
                    (void)co_await two->done2.async_wait(1s);
                }
                (void)co_await client.async_wait_reader_stopped(2s);
                shared->client_done.set();
                co_return;
            }
#
            if (two->rc1 != 0) {
                shared->client_rc = two->rc1;
                client.stop();
                (void)co_await client.async_wait_reader_stopped(2s);
                shared->client_done.set();
                co_return;
            }
            if (two->rc2 != 0) {
                shared->client_rc = two->rc2;
                client.stop();
                (void)co_await client.async_wait_reader_stopped(2s);
                shared->client_done.set();
                co_return;
            }
#
            client.stop();
            (void)co_await client.async_wait_reader_stopped(2s);
            shared->client_rc = 0;
            shared->client_done.set();
            co_return;
        },
        asio::detached);
#
    auto ec = co_await shared->server_done.async_wait(3s);
    if (ec) {
        std::cerr << "[HSMS][tcp][loopback][reorder] 等待 server_done 失败: "
                  << ec.message() << "\n";
        co_return 4;
    }
    ec = co_await shared->client_done.async_wait(3s);
    if (ec) {
        std::cerr << "[HSMS][tcp][loopback][reorder] 等待 client_done 失败: "
                  << ec.message() << "\n";
        co_return 5;
    }
#
    if (shared->server_rc != 0) {
        co_return shared->server_rc;
    }
    if (shared->client_rc != 0) {
        co_return shared->client_rc;
    }
#
    co_return 0;
}
#
static asio::awaitable<int> case_t3_timeout_and_late_response() {
    auto ex = co_await asio::this_coro::executor;
    constexpr std::uint16_t device_id = 1;
#
    auto [acc_ec, acceptor] = make_loopback_acceptor(ex);
    if (acc_ec) {
        if (is_socket_not_permitted(acc_ec)) {
            std::cerr << "[HSMS][tcp][loopback][t3] socket 被禁用，跳过\n";
            co_return 77;
        }
        std::cerr << "[HSMS][tcp][loopback][t3] listen 失败: " << acc_ec.message()
                  << "\n";
        co_return 2;
    }
#
    std::error_code ep_ec;
    const auto ep = acceptor.local_endpoint(ep_ec);
    if (ep_ec) {
        std::cerr << "[HSMS][tcp][loopback][t3] local_endpoint 失败: "
                  << ep_ec.message() << "\n";
        co_return 3;
    }
#
    const auto server_opts = make_default_opts(device_id);
    auto client_opts = make_default_opts(device_id);
    client_opts.t3 = 150ms;
#
    struct Shared final {
        secs::core::Event server_done{};
        secs::core::Event client_done{};
        int server_rc{1};
        int client_rc{1};
    };
    auto shared = std::make_shared<Shared>();
#
    asio::co_spawn(
        ex,
        [acc = std::move(acceptor), shared, server_opts]() mutable
        -> asio::awaitable<void> {
            auto [accept_ec, socket] = co_await async_accept_one(acc);
            if (accept_ec) {
                shared->server_rc = 10;
                std::cerr << "[HSMS][tcp][loopback][t3] accept 失败: "
                          << accept_ec.message() << "\n";
                shared->server_done.set();
                co_return;
            }
#
            secs::hsms::Session server(acc.get_executor(), server_opts);
            auto ec = co_await server.async_open_passive(std::move(socket));
            if (ec) {
                shared->server_rc = 11;
                std::cerr << "[HSMS][tcp][loopback][t3] server SELECT 失败: "
                          << ec.message() << "\n";
                server.stop();
                (void)co_await server.async_wait_reader_stopped(2s);
                shared->server_done.set();
                co_return;
            }
#
            auto [rec, req] = co_await server.async_receive_data(2s);
            if (rec) {
                shared->server_rc = 12;
                std::cerr << "[HSMS][tcp][loopback][t3] server recv 失败: "
                          << rec.message() << "\n";
                server.stop();
                (void)co_await server.async_wait_reader_stopped(2s);
                shared->server_done.set();
                co_return;
            }
#
            asio::steady_timer t(acc.get_executor());
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
                shared->server_rc = 13;
                std::cerr << "[HSMS][tcp][loopback][t3] server send 失败: "
                          << ec.message() << "\n";
                server.stop();
                (void)co_await server.async_wait_reader_stopped(2s);
                shared->server_done.set();
                co_return;
            }
#
            server.stop();
            (void)co_await server.async_wait_reader_stopped(2s);
            shared->server_rc = 0;
            shared->server_done.set();
            co_return;
        },
        asio::detached);
#
    asio::co_spawn(
        ex,
        [shared, client_opts, ep]() -> asio::awaitable<void> {
            auto ex2 = co_await asio::this_coro::executor;
            secs::hsms::Session client(ex2, client_opts);
#
            auto ec = co_await client.async_open_active(ep);
            if (ec) {
                shared->client_rc = 20;
                std::cerr << "[HSMS][tcp][loopback][t3] client SELECT 失败: "
                          << ec.message() << "\n";
                client.stop();
                (void)co_await client.async_wait_reader_stopped(2s);
                shared->client_done.set();
                co_return;
            }
#
            const auto item = make_test_item(/*tag=*/333U);
            auto enc = encode_item(item);
            if (enc.first) {
                shared->client_rc = 21;
                std::cerr << "[HSMS][tcp][loopback][t3] client 编码失败: "
                          << enc.first.message() << "\n";
                client.stop();
                (void)co_await client.async_wait_reader_stopped(2s);
                shared->client_done.set();
                co_return;
            }
#
            auto req = co_await client.async_request_data(
                1,
                13,
                bytes_view{enc.second.data(), enc.second.size()},
                /*timeout=*/client_opts.t3);
            if (req.first != make_error_code(errc::timeout)) {
                shared->client_rc = 22;
                std::cerr << "[HSMS][tcp][loopback][t3] 期望 timeout，实际: ["
                          << req.first.value() << "] " << req.first.message()
                          << "\n";
                client.stop();
                (void)co_await client.async_wait_reader_stopped(2s);
                shared->client_done.set();
                co_return;
            }
#
            auto late = co_await client.async_receive_data(1s);
            if (late.first) {
                shared->client_rc = 23;
                std::cerr << "[HSMS][tcp][loopback][t3] 未收到 late response: "
                          << late.first.message() << "\n";
                client.stop();
                (void)co_await client.async_wait_reader_stopped(2s);
                shared->client_done.set();
                co_return;
            }
            auto dec = decode_item(bytes_view{late.second.body.data(),
                                              late.second.body.size()});
            if (dec.first || dec.second != item) {
                shared->client_rc = 24;
                std::cerr << "[HSMS][tcp][loopback][t3] late response 解码或内容不符\n";
                client.stop();
                (void)co_await client.async_wait_reader_stopped(2s);
                shared->client_done.set();
                co_return;
            }
#
            client.stop();
            (void)co_await client.async_wait_reader_stopped(2s);
            shared->client_rc = 0;
            shared->client_done.set();
            co_return;
        },
        asio::detached);
#
    auto ec = co_await shared->server_done.async_wait(3s);
    if (ec) {
        std::cerr << "[HSMS][tcp][loopback][t3] 等待 server_done 失败: "
                  << ec.message() << "\n";
        co_return 4;
    }
    ec = co_await shared->client_done.async_wait(3s);
    if (ec) {
        std::cerr << "[HSMS][tcp][loopback][t3] 等待 client_done 失败: "
                  << ec.message() << "\n";
        co_return 5;
    }
#
    if (shared->server_rc != 0) {
        co_return shared->server_rc;
    }
    if (shared->client_rc != 0) {
        co_return shared->client_rc;
    }
#
    co_return 0;
}
#
static asio::awaitable<int> case_separate_disconnect() {
    auto ex = co_await asio::this_coro::executor;
    constexpr std::uint16_t device_id = 1;
#
    auto [acc_ec, acceptor] = make_loopback_acceptor(ex);
    if (acc_ec) {
        if (is_socket_not_permitted(acc_ec)) {
            std::cerr << "[HSMS][tcp][loopback][separate] socket 被禁用，跳过\n";
            co_return 77;
        }
        std::cerr << "[HSMS][tcp][loopback][separate] listen 失败: "
                  << acc_ec.message() << "\n";
        co_return 2;
    }
#
    std::error_code ep_ec;
    const auto ep = acceptor.local_endpoint(ep_ec);
    if (ep_ec) {
        std::cerr << "[HSMS][tcp][loopback][separate] local_endpoint 失败: "
                  << ep_ec.message() << "\n";
        co_return 3;
    }
#
    const auto server_opts = make_default_opts(device_id);
    const auto client_opts = make_default_opts(device_id);
#
    struct Shared final {
        secs::core::Event server_done{};
        secs::core::Event client_done{};
        int server_rc{1};
        int client_rc{1};
    };
    auto shared = std::make_shared<Shared>();
#
    asio::co_spawn(
        ex,
        [acc = std::move(acceptor), shared, server_opts]() mutable
        -> asio::awaitable<void> {
            auto [accept_ec, socket] = co_await async_accept_one(acc);
            if (accept_ec) {
                shared->server_rc = 10;
                std::cerr << "[HSMS][tcp][loopback][separate] accept 失败: "
                          << accept_ec.message() << "\n";
                shared->server_done.set();
                co_return;
            }
#
            secs::hsms::Session server(acc.get_executor(), server_opts);
            auto ec = co_await server.async_open_passive(std::move(socket));
            if (ec) {
                shared->server_rc = 11;
                std::cerr << "[HSMS][tcp][loopback][separate] server SELECT 失败: "
                          << ec.message() << "\n";
                server.stop();
                (void)co_await server.async_wait_reader_stopped(2s);
                shared->server_done.set();
                co_return;
            }
#
            const auto sep = secs::hsms::make_separate_req(
                /*session_id=*/0xFFFF,
                server.allocate_system_bytes());
            ec = co_await server.async_send(sep);
            if (ec) {
                shared->server_rc = 12;
                std::cerr
                    << "[HSMS][tcp][loopback][separate] server send SEPARATE 失败: "
                    << ec.message() << "\n";
                server.stop();
                (void)co_await server.async_wait_reader_stopped(2s);
                shared->server_done.set();
                co_return;
            }
#
            (void)co_await server.async_wait_reader_stopped(2s);
            server.stop();
            (void)co_await server.async_wait_reader_stopped(2s);
            shared->server_rc = 0;
            shared->server_done.set();
            co_return;
        },
        asio::detached);
#
    asio::co_spawn(
        ex,
        [shared, client_opts, ep]() -> asio::awaitable<void> {
            secs::hsms::Session client(co_await asio::this_coro::executor,
                                       client_opts);
#
            auto ec = co_await client.async_open_active(ep);
            if (ec) {
                shared->client_rc = 20;
                std::cerr << "[HSMS][tcp][loopback][separate] client SELECT 失败: "
                          << ec.message() << "\n";
                client.stop();
                (void)co_await client.async_wait_reader_stopped(2s);
                shared->client_done.set();
                co_return;
            }
#
            (void)co_await client.async_wait_reader_stopped(2s);
            auto [rec, _] = co_await client.async_receive_data(std::nullopt);
            if (rec != make_error_code(errc::cancelled)) {
                shared->client_rc = 21;
                std::cerr << "[HSMS][tcp][loopback][separate] 期望 cancelled，实际: ["
                          << rec.value() << "] " << rec.message() << "\n";
                client.stop();
                (void)co_await client.async_wait_reader_stopped(2s);
                shared->client_done.set();
                co_return;
            }
#
            client.stop();
            (void)co_await client.async_wait_reader_stopped(2s);
            shared->client_rc = 0;
            shared->client_done.set();
            co_return;
        },
        asio::detached);
#
    auto ec = co_await shared->server_done.async_wait(3s);
    if (ec) {
        std::cerr << "[HSMS][tcp][loopback][separate] 等待 server_done 失败: "
                  << ec.message() << "\n";
        co_return 4;
    }
    ec = co_await shared->client_done.async_wait(3s);
    if (ec) {
        std::cerr << "[HSMS][tcp][loopback][separate] 等待 client_done 失败: "
                  << ec.message() << "\n";
        co_return 5;
    }
#
    if (shared->server_rc != 0) {
        co_return shared->server_rc;
    }
    if (shared->client_rc != 0) {
        co_return shared->client_rc;
    }
#
    co_return 0;
}
#
static asio::awaitable<int> case_inbound_overflow_disconnect() {
    auto ex = co_await asio::this_coro::executor;
    constexpr std::uint16_t device_id = 1;
#
    auto [acc_ec, acceptor] = make_loopback_acceptor(ex);
    if (acc_ec) {
        if (is_socket_not_permitted(acc_ec)) {
            std::cerr << "[HSMS][tcp][loopback][inbound_overflow] socket 被禁用，跳过\n";
            co_return 77;
        }
        std::cerr << "[HSMS][tcp][loopback][inbound_overflow] listen 失败: "
                  << acc_ec.message() << "\n";
        co_return 2;
    }
#
    std::error_code ep_ec;
    const auto ep = acceptor.local_endpoint(ep_ec);
    if (ep_ec) {
        std::cerr << "[HSMS][tcp][loopback][inbound_overflow] local_endpoint 失败: "
                  << ep_ec.message() << "\n";
        co_return 3;
    }
#
    const auto server_opts = make_default_opts(device_id);
    auto client_opts = make_default_opts(device_id);
    client_opts.max_inbound_data_messages = 2;
#
    struct Shared final {
        secs::core::Event server_done{};
        secs::core::Event client_done{};
        int server_rc{1};
        int client_rc{1};
    };
    auto shared = std::make_shared<Shared>();
#
    asio::co_spawn(
        ex,
        [acc = std::move(acceptor), shared, server_opts]() mutable
        -> asio::awaitable<void> {
            auto [accept_ec, socket] = co_await async_accept_one(acc);
            if (accept_ec) {
                shared->server_rc = 10;
                std::cerr << "[HSMS][tcp][loopback][inbound_overflow] accept 失败: "
                          << accept_ec.message() << "\n";
                shared->server_done.set();
                co_return;
            }
#
            secs::hsms::Session server(acc.get_executor(), server_opts);
            auto ec = co_await server.async_open_passive(std::move(socket));
            if (ec) {
                shared->server_rc = 11;
                std::cerr
                    << "[HSMS][tcp][loopback][inbound_overflow] server SELECT 失败: "
                    << ec.message() << "\n";
                server.stop();
                (void)co_await server.async_wait_reader_stopped(2s);
                shared->server_done.set();
                co_return;
            }
#
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
            (void)co_await server.async_wait_reader_stopped(2s);
            shared->server_rc = 0;
            shared->server_done.set();
            co_return;
        },
        asio::detached);
#
    asio::co_spawn(
        ex,
        [shared, client_opts, ep]() -> asio::awaitable<void> {
            secs::hsms::Session client(co_await asio::this_coro::executor,
                                       client_opts);
#
            auto ec = co_await client.async_open_active(ep);
            if (ec) {
                shared->client_rc = 20;
                std::cerr
                    << "[HSMS][tcp][loopback][inbound_overflow] client SELECT 失败: "
                    << ec.message() << "\n";
                client.stop();
                (void)co_await client.async_wait_reader_stopped(2s);
                shared->client_done.set();
                co_return;
            }
#
            ec = co_await client.async_wait_reader_stopped(2s);
            if (ec) {
                shared->client_rc = 21;
                std::cerr
                    << "[HSMS][tcp][loopback][inbound_overflow] 等待 reader_stopped 失败: "
                    << ec.message() << "\n";
                client.stop();
                (void)co_await client.async_wait_reader_stopped(2s);
                shared->client_done.set();
                co_return;
            }
            auto [rec, _] = co_await client.async_receive_data(std::nullopt);
            if (rec != make_error_code(errc::buffer_overflow)) {
                shared->client_rc = 22;
                std::cerr
                    << "[HSMS][tcp][loopback][inbound_overflow] 期望 buffer_overflow，实际: ["
                    << rec.value() << "] " << rec.message() << "\n";
                client.stop();
                (void)co_await client.async_wait_reader_stopped(2s);
                shared->client_done.set();
                co_return;
            }
#
            client.stop();
            (void)co_await client.async_wait_reader_stopped(2s);
            shared->client_rc = 0;
            shared->client_done.set();
            co_return;
        },
        asio::detached);
#
    auto ec = co_await shared->server_done.async_wait(3s);
    if (ec) {
        std::cerr << "[HSMS][tcp][loopback][inbound_overflow] 等待 server_done 失败: "
                  << ec.message() << "\n";
        co_return 4;
    }
    ec = co_await shared->client_done.async_wait(3s);
    if (ec) {
        std::cerr << "[HSMS][tcp][loopback][inbound_overflow] 等待 client_done 失败: "
                  << ec.message() << "\n";
        co_return 5;
    }
#
    if (shared->server_rc != 0) {
        co_return shared->server_rc;
    }
    if (shared->client_rc != 0) {
        co_return shared->client_rc;
    }
#
    co_return 0;
}
#
static asio::awaitable<int> case_pending_overflow_is_nonfatal() {
    auto ex = co_await asio::this_coro::executor;
    constexpr std::uint16_t device_id = 1;
#
    auto [acc_ec, acceptor] = make_loopback_acceptor(ex);
    if (acc_ec) {
        if (is_socket_not_permitted(acc_ec)) {
            std::cerr << "[HSMS][tcp][loopback][pending_overflow] socket 被禁用，跳过\n";
            co_return 77;
        }
        std::cerr << "[HSMS][tcp][loopback][pending_overflow] listen 失败: "
                  << acc_ec.message() << "\n";
        co_return 2;
    }
#
    std::error_code ep_ec;
    const auto ep = acceptor.local_endpoint(ep_ec);
    if (ep_ec) {
        std::cerr << "[HSMS][tcp][loopback][pending_overflow] local_endpoint 失败: "
                  << ep_ec.message() << "\n";
        co_return 3;
    }
#
    const auto server_opts = make_default_opts(device_id);
    auto client_opts = make_default_opts(device_id);
    client_opts.max_pending_requests = 2;
#
    struct Shared final {
        secs::core::Event two_received{};
        secs::core::Event allow_response{};
        secs::core::Event client_done{};
        secs::core::Event server_done{};
        secs::core::Event client_done_evt{};
        int server_rc{1};
        int client_rc{1};
    };
    auto shared = std::make_shared<Shared>();
#
    asio::co_spawn(
        ex,
        [acc = std::move(acceptor), shared, server_opts]() mutable
        -> asio::awaitable<void> {
            auto [accept_ec, socket] = co_await async_accept_one(acc);
            if (accept_ec) {
                shared->server_rc = 10;
                std::cerr << "[HSMS][tcp][loopback][pending_overflow] accept 失败: "
                          << accept_ec.message() << "\n";
                shared->server_done.set();
                co_return;
            }
#
            secs::hsms::Session server(acc.get_executor(), server_opts);
            auto ec = co_await server.async_open_passive(std::move(socket));
            if (ec) {
                shared->server_rc = 11;
                std::cerr
                    << "[HSMS][tcp][loopback][pending_overflow] server SELECT 失败: "
                    << ec.message() << "\n";
                server.stop();
                (void)co_await server.async_wait_reader_stopped(2s);
                shared->server_done.set();
                co_return;
            }
#
            auto [rec1, req1] = co_await server.async_receive_data(2s);
            if (rec1) {
                shared->server_rc = 12;
                std::cerr
                    << "[HSMS][tcp][loopback][pending_overflow] recv#1 失败: "
                    << rec1.message() << "\n";
                server.stop();
                (void)co_await server.async_wait_reader_stopped(2s);
                shared->server_done.set();
                co_return;
            }
            auto [rec2, req2] = co_await server.async_receive_data(2s);
            if (rec2) {
                shared->server_rc = 13;
                std::cerr
                    << "[HSMS][tcp][loopback][pending_overflow] recv#2 失败: "
                    << rec2.message() << "\n";
                server.stop();
                (void)co_await server.async_wait_reader_stopped(2s);
                shared->server_done.set();
                co_return;
            }
            shared->two_received.set();
#
            (void)co_await shared->allow_response.async_wait(2s);
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
                shared->server_rc = 14;
                std::cerr
                    << "[HSMS][tcp][loopback][pending_overflow] send#1 失败: "
                    << ec.message() << "\n";
            }
            ec = co_await server.async_send(rsp2);
            if (ec) {
                shared->server_rc = 15;
                std::cerr
                    << "[HSMS][tcp][loopback][pending_overflow] send#2 失败: "
                    << ec.message() << "\n";
            }
#
            (void)co_await shared->client_done.async_wait(3s);
            server.stop();
            (void)co_await server.async_wait_reader_stopped(2s);
            shared->server_rc = 0;
            shared->server_done.set();
            co_return;
        },
        asio::detached);
#
    asio::co_spawn(
        ex,
        [shared, client_opts, ep]() -> asio::awaitable<void> {
            auto ex2 = co_await asio::this_coro::executor;
            secs::hsms::Session client(ex2, client_opts);
            bool requests_started = false;
#
            auto ec = co_await client.async_open_active(ep);
            if (ec) {
                shared->client_rc = 20;
                std::cerr
                    << "[HSMS][tcp][loopback][pending_overflow] client SELECT 失败: "
                    << ec.message() << "\n";
                shared->client_done.set();
                client.stop();
                (void)co_await client.async_wait_reader_stopped(2s);
                shared->client_done_evt.set();
                co_return;
            }
#
            struct TwoReq final {
                secs::core::Event done1{};
                secs::core::Event done2{};
                int rc1{1};
                int rc2{1};
            };
            auto two = std::make_shared<TwoReq>();
#
            asio::co_spawn(
                ex2,
                [&client, two]() -> asio::awaitable<void> {
                    const auto item = make_test_item(/*tag=*/1001U);
                    auto enc = encode_item(item);
                    if (enc.first) {
                        two->rc1 = 30;
                        two->done1.set();
                        co_return;
                    }
                    auto req = co_await client.async_request_data(
                        1,
                        13,
                        bytes_view{enc.second.data(), enc.second.size()},
                        2s);
                    if (req.first) {
                        two->rc1 = 31;
                        two->done1.set();
                        co_return;
                    }
                    auto dec = decode_item(bytes_view{req.second.body.data(),
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
                ex2,
                [&client, two]() -> asio::awaitable<void> {
                    const auto item = make_test_item(/*tag=*/1002U);
                    auto enc = encode_item(item);
                    if (enc.first) {
                        two->rc2 = 40;
                        two->done2.set();
                        co_return;
                    }
                    auto req = co_await client.async_request_data(
                        1,
                        13,
                        bytes_view{enc.second.data(), enc.second.size()},
                        2s);
                    if (req.first) {
                        two->rc2 = 41;
                        two->done2.set();
                        co_return;
                    }
                    auto dec = decode_item(bytes_view{req.second.body.data(),
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
            requests_started = true;
#
            ec = co_await shared->two_received.async_wait(2s);
            if (ec) {
                shared->client_rc = 21;
                std::cerr
                    << "[HSMS][tcp][loopback][pending_overflow] 等待 two_received 失败: "
                    << ec.message() << "\n";
                shared->allow_response.set();
                shared->client_done.set();
                client.stop();
                if (requests_started) {
                    (void)co_await two->done1.async_wait(1s);
                    (void)co_await two->done2.async_wait(1s);
                }
                (void)co_await client.async_wait_reader_stopped(2s);
                shared->client_done_evt.set();
                co_return;
            }
#
            // 第三条请求应因 pending 上限快速失败（buffer_overflow），不应导致断线。
            const auto item = make_test_item(/*tag=*/1003U);
            auto enc = encode_item(item);
            if (enc.first) {
                shared->client_rc = 22;
                shared->allow_response.set();
                shared->client_done.set();
                client.stop();
                if (requests_started) {
                    (void)co_await two->done1.async_wait(1s);
                    (void)co_await two->done2.async_wait(1s);
                }
                (void)co_await client.async_wait_reader_stopped(2s);
                shared->client_done_evt.set();
                co_return;
            }
#
            auto req = co_await client.async_request_data(
                1,
                13,
                bytes_view{enc.second.data(), enc.second.size()},
                2s);
            if (req.first != make_error_code(errc::buffer_overflow)) {
                shared->client_rc = 23;
                std::cerr
                    << "[HSMS][tcp][loopback][pending_overflow] 期望 buffer_overflow，实际: ["
                    << req.first.value() << "] " << req.first.message() << "\n";
            }
#
            shared->allow_response.set();
#
            (void)co_await two->done1.async_wait(2s);
            (void)co_await two->done2.async_wait(2s);
            if (shared->client_rc == 1) {
                if (two->rc1 != 0) {
                    shared->client_rc = two->rc1;
                } else if (two->rc2 != 0) {
                    shared->client_rc = two->rc2;
                } else {
                    auto lec = co_await client.async_linktest();
                    if (lec) {
                        shared->client_rc = 24;
                        std::cerr
                            << "[HSMS][tcp][loopback][pending_overflow] 后续 LINKTEST 失败: "
                            << lec.message() << "\n";
                    } else {
                        shared->client_rc = 0;
                    }
                }
            }
#
            shared->client_done.set();
            client.stop();
            (void)co_await client.async_wait_reader_stopped(2s);
            shared->client_done_evt.set();
            co_return;
        },
        asio::detached);
#
    auto ec = co_await shared->server_done.async_wait(4s);
    if (ec) {
        std::cerr << "[HSMS][tcp][loopback][pending_overflow] 等待 server_done 失败: "
                  << ec.message() << "\n";
        co_return 4;
    }
    ec = co_await shared->client_done_evt.async_wait(4s);
    if (ec) {
        std::cerr
            << "[HSMS][tcp][loopback][pending_overflow] 等待 client_done 失败: "
            << ec.message() << "\n";
        co_return 5;
    }
#
    if (shared->server_rc != 0) {
        co_return shared->server_rc;
    }
    if (shared->client_rc != 0) {
        co_return shared->client_rc;
    }
#
    co_return 0;
}
#
static asio::awaitable<int> case_stop_during_pending_request() {
    auto ex = co_await asio::this_coro::executor;
    constexpr std::uint16_t device_id = 1;
#
    auto [acc_ec, acceptor] = make_loopback_acceptor(ex);
    if (acc_ec) {
        if (is_socket_not_permitted(acc_ec)) {
            std::cerr << "[HSMS][tcp][loopback][stop_pending] socket 被禁用，跳过\n";
            co_return 77;
        }
        std::cerr << "[HSMS][tcp][loopback][stop_pending] listen 失败: "
                  << acc_ec.message() << "\n";
        co_return 2;
    }
#
    std::error_code ep_ec;
    const auto ep = acceptor.local_endpoint(ep_ec);
    if (ep_ec) {
        std::cerr << "[HSMS][tcp][loopback][stop_pending] local_endpoint 失败: "
                  << ep_ec.message() << "\n";
        co_return 3;
    }
#
    const auto server_opts = make_default_opts(device_id);
    auto client_opts = make_default_opts(device_id);
    client_opts.t3 = 2s;
#
    struct Shared final {
        secs::core::Event got_req{};
        secs::core::Event client_done{};
        secs::core::Event server_done{};
        secs::core::Event client_done_evt{};
        int server_rc{1};
        int client_rc{1};
    };
    auto shared = std::make_shared<Shared>();
#
    asio::co_spawn(
        ex,
        [acc = std::move(acceptor), shared, server_opts]() mutable
        -> asio::awaitable<void> {
            auto [accept_ec, socket] = co_await async_accept_one(acc);
            if (accept_ec) {
                shared->server_rc = 10;
                std::cerr << "[HSMS][tcp][loopback][stop_pending] accept 失败: "
                          << accept_ec.message() << "\n";
                shared->server_done.set();
                co_return;
            }
#
            secs::hsms::Session server(acc.get_executor(), server_opts);
            auto ec = co_await server.async_open_passive(std::move(socket));
            if (ec) {
                shared->server_rc = 11;
                std::cerr << "[HSMS][tcp][loopback][stop_pending] server SELECT 失败: "
                          << ec.message() << "\n";
                server.stop();
                (void)co_await server.async_wait_reader_stopped(2s);
                shared->server_done.set();
                co_return;
            }
#
            auto [rec, _] = co_await server.async_receive_data(2s);
            if (rec) {
                shared->server_rc = 12;
                std::cerr << "[HSMS][tcp][loopback][stop_pending] server recv 失败: "
                          << rec.message() << "\n";
                server.stop();
                (void)co_await server.async_wait_reader_stopped(2s);
                shared->server_done.set();
                co_return;
            }
            shared->got_req.set();
#
            (void)co_await shared->client_done.async_wait(3s);
            server.stop();
            (void)co_await server.async_wait_reader_stopped(2s);
            shared->server_rc = 0;
            shared->server_done.set();
            co_return;
        },
        asio::detached);
#
    asio::co_spawn(
        ex,
        [shared, client_opts, ep]() -> asio::awaitable<void> {
            auto ex2 = co_await asio::this_coro::executor;
            secs::hsms::Session client(ex2, client_opts);
#
            auto ec = co_await client.async_open_active(ep);
            if (ec) {
                shared->client_rc = 20;
                std::cerr << "[HSMS][tcp][loopback][stop_pending] client SELECT 失败: "
                          << ec.message() << "\n";
                shared->client_done.set();
                shared->client_done_evt.set();
                co_return;
            }
#
            struct Result final {
                secs::core::Event done{};
                std::error_code ec{};
            };
            auto result = std::make_shared<Result>();
#
            asio::co_spawn(
                ex2,
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
            ec = co_await shared->got_req.async_wait(2s);
            if (ec) {
                shared->client_rc = 21;
                std::cerr << "[HSMS][tcp][loopback][stop_pending] 等待 got_req 失败: "
                          << ec.message() << "\n";
                shared->client_done.set();
                client.stop();
                (void)co_await client.async_wait_reader_stopped(2s);
                shared->client_done_evt.set();
                co_return;
            }
#
            client.stop();
            (void)co_await client.async_wait_reader_stopped(2s);
#
            ec = co_await result->done.async_wait(2s);
            if (ec) {
                shared->client_rc = 22;
                std::cerr << "[HSMS][tcp][loopback][stop_pending] 等待 request 完成失败: "
                          << ec.message() << "\n";
            } else if (result->ec != make_error_code(errc::cancelled)) {
                shared->client_rc = 23;
                std::cerr << "[HSMS][tcp][loopback][stop_pending] 期望 cancelled，实际: ["
                          << result->ec.value() << "] " << result->ec.message()
                          << "\n";
            } else {
                shared->client_rc = 0;
            }
#
            shared->client_done.set();
            shared->client_done_evt.set();
            co_return;
        },
        asio::detached);
#
    auto ec = co_await shared->server_done.async_wait(4s);
    if (ec) {
        std::cerr << "[HSMS][tcp][loopback][stop_pending] 等待 server_done 失败: "
                  << ec.message() << "\n";
        co_return 4;
    }
    ec = co_await shared->client_done_evt.async_wait(4s);
    if (ec) {
        std::cerr << "[HSMS][tcp][loopback][stop_pending] 等待 client_done 失败: "
                  << ec.message() << "\n";
        co_return 5;
    }
#
    if (shared->server_rc != 0) {
        co_return shared->server_rc;
    }
    if (shared->client_rc != 0) {
        co_return shared->client_rc;
    }
#
    co_return 0;
}
#
static asio::awaitable<int> case_auto_reconnect_after_separate() {
    auto ex = co_await asio::this_coro::executor;
    constexpr std::uint16_t device_id = 1;
#
    auto [acc_ec, acceptor] = make_loopback_acceptor(ex);
    if (acc_ec) {
        if (is_socket_not_permitted(acc_ec)) {
            std::cerr << "[HSMS][tcp][loopback][reconnect] socket 被禁用，跳过\n";
            co_return 77;
        }
        std::cerr << "[HSMS][tcp][loopback][reconnect] listen 失败: "
                  << acc_ec.message() << "\n";
        co_return 2;
    }
#
    std::error_code ep_ec{};
    const auto ep = acceptor.local_endpoint(ep_ec);
    if (ep_ec) {
        std::cerr << "[HSMS][tcp][loopback][reconnect] local_endpoint 失败: "
                  << ep_ec.message() << "\n";
        co_return 3;
    }
#
    auto server_opts = make_default_opts(device_id);
    auto client_opts = make_default_opts(device_id);
#
    // 让重连更快一些，避免用例跑太久。
    server_opts.auto_reconnect = true;
    server_opts.t5 = 50ms;
    client_opts.auto_reconnect = true;
    client_opts.t5 = 50ms;
#
    struct Shared final {
        secs::core::Event server_done{};
        secs::core::Event client_done{};
        secs::core::Event server_run_done{};
        secs::core::Event client_run_done{};
        std::error_code server_run_ec{};
        std::error_code client_run_ec{};
        int server_rc{1};
        int client_rc{1};
    };
    auto shared = std::make_shared<Shared>();
#
    asio::co_spawn(
        ex,
        [&, shared, server_opts]() -> asio::awaitable<void> {
            secs::hsms::Session server(co_await asio::this_coro::executor,
                                       server_opts);
#
            asio::co_spawn(
                co_await asio::this_coro::executor,
                [&]() -> asio::awaitable<void> {
                    shared->server_run_ec =
                        co_await server.async_run_passive(acceptor);
                    shared->server_run_done.set();
                },
                asio::detached);
#
            // round#1：等待首次 selected，收一条请求并回包
            auto ec = co_await server.async_wait_selected(/*min_generation=*/1, 3s);
            if (ec) {
                shared->server_rc = 10;
                std::cerr << "[HSMS][tcp][loopback][reconnect] server 等待 selected#1 失败: "
                          << ec.message() << "\n";
                std::error_code ignore{};
                acceptor.close(ignore);
                server.stop();
                (void)co_await shared->server_run_done.async_wait(2s);
                (void)co_await server.async_wait_reader_stopped(2s);
                shared->server_done.set();
                co_return;
            }
#
            auto [rec1, req1] = co_await server.async_receive_data(2s);
            if (rec1) {
                shared->server_rc = 11;
                std::cerr << "[HSMS][tcp][loopback][reconnect] server recv#1 失败: "
                          << rec1.message() << "\n";
                std::error_code ignore{};
                acceptor.close(ignore);
                server.stop();
                (void)co_await shared->server_run_done.async_wait(2s);
                (void)co_await server.async_wait_reader_stopped(2s);
                shared->server_done.set();
                co_return;
            }
#
            const auto rsp1 =
                secs::hsms::make_data_message(server_opts.session_id,
                                              req1.stream(),
                                              static_cast<std::uint8_t>(
                                                  req1.function() + 1U),
                                              /*w_bit=*/false,
                                              req1.header.system_bytes,
                                              bytes_view{req1.body.data(),
                                                         req1.body.size()});
            ec = co_await server.async_send(rsp1);
            if (ec) {
                shared->server_rc = 12;
                std::cerr << "[HSMS][tcp][loopback][reconnect] server send#1 失败: "
                          << ec.message() << "\n";
                std::error_code ignore{};
                acceptor.close(ignore);
                server.stop();
                (void)co_await shared->server_run_done.async_wait(2s);
                (void)co_await server.async_wait_reader_stopped(2s);
                shared->server_done.set();
                co_return;
            }
#
            // 主动触发断线：发 SEPARATE.req，让对端断开并触发双方进入 disconnected。
            const auto sep =
                secs::hsms::make_separate_req(/*session_id=*/0xFFFF,
                                              server.allocate_system_bytes());
            ec = co_await server.async_send(sep);
            if (ec) {
                shared->server_rc = 13;
                std::cerr << "[HSMS][tcp][loopback][reconnect] server send SEPARATE 失败: "
                          << ec.message() << "\n";
                std::error_code ignore{};
                acceptor.close(ignore);
                server.stop();
                (void)co_await shared->server_run_done.async_wait(2s);
                (void)co_await server.async_wait_reader_stopped(2s);
                shared->server_done.set();
                co_return;
            }
#
            // round#2：等待下一次 selected（重连成功）
            ec = co_await server.async_wait_selected(/*min_generation=*/2, 3s);
            if (ec) {
                shared->server_rc = 14;
                std::cerr << "[HSMS][tcp][loopback][reconnect] server 等待 selected#2 失败: "
                          << ec.message() << "\n";
                std::error_code ignore{};
                acceptor.close(ignore);
                server.stop();
                (void)co_await shared->server_run_done.async_wait(2s);
                (void)co_await server.async_wait_reader_stopped(2s);
                shared->server_done.set();
                co_return;
            }
#
            auto [rec2, req2] = co_await server.async_receive_data(2s);
            if (rec2) {
                shared->server_rc = 15;
                std::cerr << "[HSMS][tcp][loopback][reconnect] server recv#2 失败: "
                          << rec2.message() << "\n";
                std::error_code ignore{};
                acceptor.close(ignore);
                server.stop();
                (void)co_await shared->server_run_done.async_wait(2s);
                (void)co_await server.async_wait_reader_stopped(2s);
                shared->server_done.set();
                co_return;
            }
#
            const auto rsp2 =
                secs::hsms::make_data_message(server_opts.session_id,
                                              req2.stream(),
                                              static_cast<std::uint8_t>(
                                                  req2.function() + 1U),
                                              /*w_bit=*/false,
                                              req2.header.system_bytes,
                                              bytes_view{req2.body.data(),
                                                         req2.body.size()});
            ec = co_await server.async_send(rsp2);
            if (ec) {
                shared->server_rc = 16;
                std::cerr << "[HSMS][tcp][loopback][reconnect] server send#2 失败: "
                          << ec.message() << "\n";
                std::error_code ignore{};
                acceptor.close(ignore);
                server.stop();
                (void)co_await shared->server_run_done.async_wait(2s);
                (void)co_await server.async_wait_reader_stopped(2s);
                shared->server_done.set();
                co_return;
            }
#
            std::error_code ignore{};
            acceptor.close(ignore);
            server.stop();
            (void)co_await shared->server_run_done.async_wait(2s);
            (void)co_await server.async_wait_reader_stopped(2s);
            shared->server_rc = shared->server_run_ec ? 17 : 0;
            shared->server_done.set();
            co_return;
        },
        asio::detached);
#
    asio::co_spawn(
        ex,
        [shared, client_opts, ep]() -> asio::awaitable<void> {
            secs::hsms::Session client(co_await asio::this_coro::executor,
                                       client_opts);
#
            asio::co_spawn(
                co_await asio::this_coro::executor,
                [&]() -> asio::awaitable<void> {
                    shared->client_run_ec = co_await client.async_run_active(ep);
                    shared->client_run_done.set();
                },
                asio::detached);
#
            auto ec = co_await client.async_wait_selected(/*min_generation=*/1, 3s);
            if (ec) {
                shared->client_rc = 20;
                std::cerr << "[HSMS][tcp][loopback][reconnect] client 等待 selected#1 失败: "
                          << ec.message() << "\n";
                client.stop();
                (void)co_await shared->client_run_done.async_wait(2s);
                (void)co_await client.async_wait_reader_stopped(2s);
                shared->client_done.set();
                co_return;
            }
#
            const auto item1 = make_test_item(/*tag=*/3001U);
            auto [enc_ec1, body1] = encode_item(item1);
            if (enc_ec1) {
                shared->client_rc = 21;
                std::cerr << "[HSMS][tcp][loopback][reconnect] client 编码#1 失败: "
                          << enc_ec1.message() << "\n";
                client.stop();
                (void)co_await shared->client_run_done.async_wait(2s);
                (void)co_await client.async_wait_reader_stopped(2s);
                shared->client_done.set();
                co_return;
            }
#
            auto [req_ec1, rsp1] = co_await client.async_request_data(
                1, 13, bytes_view{body1.data(), body1.size()}, 2s);
            if (req_ec1) {
                shared->client_rc = 22;
                std::cerr << "[HSMS][tcp][loopback][reconnect] client request#1 失败: "
                          << req_ec1.message() << "\n";
                client.stop();
                (void)co_await shared->client_run_done.async_wait(2s);
                (void)co_await client.async_wait_reader_stopped(2s);
                shared->client_done.set();
                co_return;
            }
#
            auto [dec_ec1, out1] =
                decode_item(bytes_view{rsp1.body.data(), rsp1.body.size()});
            if (dec_ec1 || out1 != item1) {
                shared->client_rc = 23;
                std::cerr << "[HSMS][tcp][loopback][reconnect] client 回包#1 不匹配\n";
                client.stop();
                (void)co_await shared->client_run_done.async_wait(2s);
                (void)co_await client.async_wait_reader_stopped(2s);
                shared->client_done.set();
                co_return;
            }
#
            ec = co_await client.async_wait_selected(/*min_generation=*/2, 3s);
            if (ec) {
                shared->client_rc = 24;
                std::cerr << "[HSMS][tcp][loopback][reconnect] client 等待 selected#2 失败: "
                          << ec.message() << "\n";
                client.stop();
                (void)co_await shared->client_run_done.async_wait(2s);
                (void)co_await client.async_wait_reader_stopped(2s);
                shared->client_done.set();
                co_return;
            }
#
            const auto item2 = make_test_item(/*tag=*/3002U);
            auto [enc_ec2, body2] = encode_item(item2);
            if (enc_ec2) {
                shared->client_rc = 25;
                std::cerr << "[HSMS][tcp][loopback][reconnect] client 编码#2 失败: "
                          << enc_ec2.message() << "\n";
                client.stop();
                (void)co_await shared->client_run_done.async_wait(2s);
                (void)co_await client.async_wait_reader_stopped(2s);
                shared->client_done.set();
                co_return;
            }
#
            auto [req_ec2, rsp2] = co_await client.async_request_data(
                1, 13, bytes_view{body2.data(), body2.size()}, 2s);
            if (req_ec2) {
                shared->client_rc = 26;
                std::cerr << "[HSMS][tcp][loopback][reconnect] client request#2 失败: "
                          << req_ec2.message() << "\n";
                client.stop();
                (void)co_await shared->client_run_done.async_wait(2s);
                (void)co_await client.async_wait_reader_stopped(2s);
                shared->client_done.set();
                co_return;
            }
#
            auto [dec_ec2, out2] =
                decode_item(bytes_view{rsp2.body.data(), rsp2.body.size()});
            if (dec_ec2 || out2 != item2) {
                shared->client_rc = 27;
                std::cerr << "[HSMS][tcp][loopback][reconnect] client 回包#2 不匹配\n";
                client.stop();
                (void)co_await shared->client_run_done.async_wait(2s);
                (void)co_await client.async_wait_reader_stopped(2s);
                shared->client_done.set();
                co_return;
            }
#
            client.stop();
            (void)co_await shared->client_run_done.async_wait(2s);
            (void)co_await client.async_wait_reader_stopped(2s);
            shared->client_rc = shared->client_run_ec ? 28 : 0;
            shared->client_done.set();
            co_return;
        },
        asio::detached);
#
    auto ec = co_await shared->server_done.async_wait(8s);
    if (ec) {
        std::cerr << "[HSMS][tcp][loopback][reconnect] 等待 server_done 失败: "
                  << ec.message() << "\n";
        co_return 4;
    }
    ec = co_await shared->client_done.async_wait(8s);
    if (ec) {
        std::cerr << "[HSMS][tcp][loopback][reconnect] 等待 client_done 失败: "
                  << ec.message() << "\n";
        co_return 5;
    }
#
    if (shared->server_rc != 0) {
        co_return shared->server_rc;
    }
    if (shared->client_rc != 0) {
        co_return shared->client_rc;
    }
    co_return 0;
}
#
static asio::awaitable<int> run_all() {
    if (const int rc = co_await case_basic_request_response(); rc != 0) {
        co_return rc;
    }
    if (const int rc = co_await case_out_of_order_responses(); rc != 0) {
        co_return rc;
    }
    if (const int rc = co_await case_t3_timeout_and_late_response(); rc != 0) {
        co_return rc;
    }
    if (const int rc = co_await case_separate_disconnect(); rc != 0) {
        co_return rc;
    }
    if (const int rc = co_await case_inbound_overflow_disconnect(); rc != 0) {
        co_return rc;
    }
    if (const int rc = co_await case_pending_overflow_is_nonfatal(); rc != 0) {
        co_return rc;
    }
    if (const int rc = co_await case_stop_during_pending_request(); rc != 0) {
        co_return rc;
    }
    if (const int rc = co_await case_auto_reconnect_after_separate(); rc != 0) {
        co_return rc;
    }
    co_return 0;
}
#
} // namespace
#
int main() {
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
