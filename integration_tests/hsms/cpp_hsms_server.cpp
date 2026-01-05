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
secs::ii::Item make_expected_item() {
    return secs::ii::Item::list({
        secs::ii::Item::u4({123U}),
        secs::ii::Item::ascii("HELLO"),
        secs::ii::Item::list({
            secs::ii::Item::u1({1U, 2U, 3U}),
        }),
    });
}
#
asio::awaitable<int> run(std::uint16_t port, std::uint16_t device_id) {
    auto ex = co_await asio::this_coro::executor;
#
    asio::ip::tcp::acceptor acceptor(ex, asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
    auto [acc_ec, socket] =
        co_await acceptor.async_accept(asio::as_tuple(asio::use_awaitable));
    if (acc_ec) {
        std::cerr << "accept 失败: " << acc_ec.message() << "\n";
        co_return 3;
    }
#
    secs::hsms::SessionOptions options{};
    options.session_id = device_id;
    options.t3 = 5s;
    options.t6 = 5s;
    options.t7 = 5s;
    options.t8 = 5s;
#
    secs::hsms::Session session(ex, options);
    {
        const auto ec = co_await session.async_open_passive(std::move(socket));
        if (ec) {
            std::cerr << "HSMS 被动打开失败: " << ec.message() << "\n";
            co_return 4;
        }
    }
#
    auto [rec_ec, req] = co_await session.async_receive_data(5s);
    if (rec_ec) {
        std::cerr << "接收数据消息失败: " << rec_ec.message() << "\n";
        co_return 5;
    }
#
    secs::ii::Item decoded = secs::ii::Item::binary({});
    std::size_t consumed = 0;
    {
        const auto dec_ec = secs::ii::decode_one(
            bytes_view{req.body.data(), req.body.size()}, decoded, consumed);
        if (dec_ec) {
            std::cerr << "请求 body 解码失败: " << dec_ec.message() << "\n";
            co_return 6;
        }
    }
    if (consumed != req.body.size()) {
        std::cerr << "请求 body 存在未消费尾部: consumed=" << consumed
                  << " total=" << req.body.size() << "\n";
        co_return 7;
    }
#
    const auto expected = make_expected_item();
    if (decoded != expected) {
        std::cerr << "入站 Item 与预期不一致\n";
        co_return 8;
    }
#
    // 回包：SxF(y+1)，system_bytes 必须回显以匹配对端挂起请求。
    const auto rsp = secs::hsms::make_data_message(
        options.session_id,
        req.stream(),
        static_cast<std::uint8_t>(req.function() + 1U),
        false,
        req.header.system_bytes,
        bytes_view{req.body.data(), req.body.size()});
#
    const auto send_ec = co_await session.async_send(rsp);
    if (send_ec) {
        std::cerr << "发送回包失败: " << send_ec.message() << "\n";
        co_return 9;
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
    if (argc != 3) {
        std::cerr << "用法: " << argv[0] << " <port> <device_id>\n";
        return 2;
    }
#
    const auto port = static_cast<std::uint16_t>(std::strtoul(argv[1], nullptr, 10));
    const auto device_id = static_cast<std::uint16_t>(std::strtoul(argv[2], nullptr, 10));
#
    asio::io_context io;
    int rc = 1;
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            rc = co_await run(port, device_id);
            io.stop();
        },
        asio::detached);
    io.run();
    return rc;
}

