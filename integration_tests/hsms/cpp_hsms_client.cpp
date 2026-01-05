#include "secs/hsms/message.hpp"
#include "secs/hsms/session.hpp"
#include "secs/ii/codec.hpp"
#include "secs/ii/item.hpp"
#
#include "secs/core/common.hpp"
#include "secs/core/error.hpp"
#
#include <asio/as_tuple.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/use_awaitable.hpp>
#
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>
#
namespace {
#
using secs::core::byte;
using secs::core::bytes_view;
#
using namespace std::chrono_literals;
#
secs::ii::Item make_test_item() {
    return secs::ii::Item::list({
        secs::ii::Item::u4({123U}),
        secs::ii::Item::ascii("HELLO"),
        secs::ii::Item::list({
            secs::ii::Item::u1({1U, 2U, 3U}),
        }),
    });
}
#
asio::awaitable<int>
run(std::string host, std::uint16_t port, std::uint16_t device_id) {
    auto ex = co_await asio::this_coro::executor;
#
    secs::hsms::SessionOptions options{};
    options.session_id = device_id;
    options.t3 = 5s;
    options.t6 = 5s;
    options.t7 = 5s;
    options.t8 = 5s;
#
    secs::hsms::Session session(ex, options);
#
    asio::ip::tcp::endpoint endpoint;
    try {
        endpoint = asio::ip::tcp::endpoint(asio::ip::make_address(host), port);
    } catch (const std::exception &e) {
        std::cerr << "解析 IP 失败: " << e.what() << "\n";
        co_return 2;
    }
#
    {
        const auto ec = co_await session.async_open_active(endpoint);
        if (ec) {
            std::cerr << "HSMS 主动连接失败: " << ec.message() << "\n";
            co_return 3;
        }
    }
#
    {
        const auto ec = co_await session.async_linktest();
        if (ec) {
            std::cerr << "LINKTEST 失败: " << ec.message() << "\n";
            co_return 4;
        }
    }
#
    const auto item = make_test_item();
    std::vector<byte> body;
    {
        const auto ec = secs::ii::encode(item, body);
        if (ec) {
            std::cerr << "SECS-II 编码失败: " << ec.message() << "\n";
            co_return 5;
        }
    }
#
    auto [ec, rsp] = co_await session.async_request_data(
        1, 13, bytes_view{body.data(), body.size()}, 5s);
    if (ec) {
        std::cerr << "请求-响应失败: " << ec.message() << "\n";
        co_return 6;
    }
#
    if (rsp.stream() != 1 || rsp.function() != 14 || rsp.w_bit()) {
        std::cerr << "响应头不符合预期: stream=" << static_cast<int>(rsp.stream())
                  << " function=" << static_cast<int>(rsp.function())
                  << " w=" << (rsp.w_bit() ? 1 : 0) << "\n";
        co_return 7;
    }
#
    secs::ii::Item decoded_item = secs::ii::Item::binary({});
    std::size_t consumed = 0;
    {
        const auto dec_ec = secs::ii::decode_one(
            bytes_view{rsp.body.data(), rsp.body.size()}, decoded_item, consumed);
        if (dec_ec) {
            std::cerr << "响应 body 解码失败: " << dec_ec.message() << "\n";
            co_return 8;
        }
    }
    if (consumed != rsp.body.size()) {
        std::cerr << "响应 body 存在未消费尾部: consumed=" << consumed
                  << " total=" << rsp.body.size() << "\n";
        co_return 9;
    }
    if (decoded_item != item) {
        std::cerr << "响应 Item 与请求不一致\n";
        co_return 10;
    }
#
    std::cout << "PASS\n";
    session.stop();
    co_return 0;
}
#
} // namespace
#
int main(int argc, char **argv) {
    if (argc != 4) {
        std::cerr << "用法: " << argv[0] << " <host> <port> <device_id>\n";
        return 2;
    }
#
    const std::string host = argv[1];
    const auto port = static_cast<std::uint16_t>(std::strtoul(argv[2], nullptr, 10));
    const auto device_id = static_cast<std::uint16_t>(std::strtoul(argv[3], nullptr, 10));
#
    asio::io_context io;
    int rc = 1;
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            rc = co_await run(host, port, device_id);
            io.stop();
        },
        asio::detached);
#
    io.run();
    return rc;
}

