/**
 * @file secs1_loopback.cpp
 * @brief SECS-I 示例：使用 MemoryLink 模拟“串口线”，跑通请求-响应
 *
 * 说明：
 * - 本示例不依赖真实串口设备，跨平台可运行；
 * - 使用 secs::secs1::StateMachine（E4 传输层）+ secs::protocol::Session（统一协议层）；
 * - payload 选择 700B，确保触发 244B/block 的分包与重组路径。
 *
 * 用法：
 *   ./secs1_loopback
 */

#include "secs/protocol/session.hpp"
#include "secs/secs1/link.hpp"
#include "secs/secs1/state_machine.hpp"

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/this_coro.hpp>

#include <chrono>
#include <cstdint>
#include <iostream>
#include <utility>
#include <vector>

using namespace secs;
using namespace std::chrono_literals;

static std::vector<core::byte> make_payload(std::size_t n) {
    std::vector<core::byte> out;
    out.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        out[i] = static_cast<core::byte>(i & 0xFFu);
    }
    return out;
}

asio::awaitable<int> run() {
    auto ex = co_await asio::this_coro::executor;

    // 1) 创建一对“内存互联”的 SECS-I Link 端点
    auto [host_link, eq_link] = secs::secs1::MemoryLink::create(ex);

    // 2) 在两端分别创建 SECS-I 传输层状态机
    constexpr std::uint16_t device_id = 1;
    secs::secs1::StateMachine host_sm(host_link, device_id);
    secs::secs1::StateMachine eq_sm(eq_link, device_id);

    // 3) 在两端创建统一协议层 Session（注意 R-bit 方向配置）
    secs::protocol::SessionOptions host_opt{};
    host_opt.t3 = 3s;
    host_opt.poll_interval = 20ms;
    host_opt.secs1_reverse_bit = false; // Host -> Equipment（R=0）

    secs::protocol::SessionOptions eq_opt = host_opt;
    eq_opt.secs1_reverse_bit = true; // Equipment -> Host（R=1）

    secs::protocol::Session host_sess(host_sm, device_id, host_opt);
    secs::protocol::Session eq_sess(eq_sm, device_id, eq_opt);

    // 设备端：注册一个回显处理器（S1F13 -> S1F14）
    eq_sess.router().set(
        1,
        13,
        [](const secs::protocol::DataMessage &msg)
            -> asio::awaitable<secs::protocol::HandlerResult> {
            co_return secs::protocol::HandlerResult{std::error_code{}, msg.body};
        });

    // 设备端：启动接收循环（负责收包、路由 handler、回包）
    asio::co_spawn(
        ex,
        [&]() -> asio::awaitable<void> { co_await eq_sess.async_run(); },
        asio::detached);

    // 4) Host 发起一次 W=1 请求并等待回应
    const auto payload = make_payload(700);
    std::cout << "[Host] sending S1F13 (W=1), payload=" << payload.size()
              << " bytes\n";

    auto [ec, rsp] = co_await host_sess.async_request(
        1,
        13,
        core::bytes_view{payload.data(), payload.size()},
        5s);

    if (ec) {
        std::cerr << "[Host] request failed: " << ec.message() << "\n";
        host_sess.stop();
        eq_sess.stop();
        co_return 1;
    }

    std::cout << "[Host] got response: S" << static_cast<int>(rsp.stream) << "F"
              << static_cast<int>(rsp.function) << " (W=" << rsp.w_bit
              << "), body=" << rsp.body.size() << " bytes\n";

    if (rsp.stream != 1 || rsp.function != 14 || rsp.w_bit) {
        std::cerr << "[Host] response header mismatch\n";
        host_sess.stop();
        eq_sess.stop();
        co_return 2;
    }
    if (rsp.body != payload) {
        std::cerr << "[Host] response payload mismatch\n";
        host_sess.stop();
        eq_sess.stop();
        co_return 3;
    }

    std::cout << "PASS\n";
    host_sess.stop();
    eq_sess.stop();
    co_return 0;
}

int main() {
    std::cout << "=== SECS-I Loopback Example (MemoryLink) ===\n\n";

    asio::io_context ioc;
    int rc = 1;

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            rc = co_await run();
            ioc.stop();
        },
        asio::detached);

    ioc.run();
    return rc;
}
