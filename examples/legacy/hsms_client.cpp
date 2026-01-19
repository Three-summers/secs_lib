/**
 * @file hsms_client.cpp
 * @brief HSMS 客户端示例 - 连接服务器并发送 SECS-II 消息
 *
 * 用法: ./hsms_client [host] [port]
 * 默认: 127.0.0.1:5000
 */

#include <secs/hsms/session.hpp>
#include <secs/ii/item.hpp>
#include <secs/protocol/session.hpp>
#include <secs/utils/protocol_helpers.hpp>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/signal_set.hpp>

#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

using namespace secs;
using namespace std::chrono_literals;

namespace {

void dump_to_stdout(void *,
                    const char *data,
                    std::size_t size) noexcept {
    if (!data || size == 0) {
        return;
    }
    (void)std::fwrite(data, 1, size, stdout);
    (void)std::fflush(stdout);
}

} // namespace

asio::awaitable<void> client_session(hsms::Session &session,
                                     protocol::Session &proto,
                                     const asio::ip::tcp::endpoint &endpoint) {

    std::cout << "[客户端] 连接到 " << endpoint << "...\n";

    // 连接并完成 SELECT 握手
    auto ec = co_await session.async_open_active(endpoint);
    if (ec) {
        std::cout << "[客户端] 连接失败: " << ec.message() << "\n";
        co_return;
    }

    std::cout << "[客户端] 已连接，会话已建立\n";

    // 发送 LINKTEST 验证连接
    ec = co_await session.async_linktest();
    if (ec) {
        std::cout << "[客户端] LINKTEST 失败: " << ec.message() << "\n";
    } else {
        std::cout << "[客户端] LINKTEST 成功\n";
    }

    // 示例 1：发送 S1F1（Are You There / 你在吗）并等待响应
    {
        std::cout << "\n[客户端] 发送 S1F1 (Are You There)...\n";

        ii::Item request_item = ii::Item::list({}); // 空 List
        auto [req_ec, out] = co_await secs::utils::async_request_decoded(
            proto,
            1, // Stream（流号）
            1, // Function（功能号）
            request_item);

        if (req_ec) {
            if (out.reply.function != 0) {
                std::cout << "[客户端] 收到 S1F2 响应，但解码失败: "
                          << req_ec.message() << "\n";
            } else {
                std::cout << "[客户端] S1F1 失败: " << req_ec.message() << "\n";
                co_return;
            }
        } else {
            std::cout << "[客户端] 收到 S1F2 响应\n";
        }

        if (out.decoded.has_value()) {
            if (auto *ascii = out.decoded->item.get_if<ii::ASCII>()) {
                std::cout << "[客户端] 响应内容: \"" << ascii->value << "\"\n";
            }
        }
    }

    // 示例 2：发送 S2F41（Host Command Send / 主机命令）并等待响应
    {
        std::cout << "\n[客户端] 发送 S2F41 (Host Command)...\n";

        // 构造 SECS-II 消息：<L <A RCMD> <L PARAMS>>
        ii::Item command =
            ii::Item::list({ii::Item::ascii("START"), // RCMD（命令名）
                            ii::Item::list({
                                // PARAMS（参数列表，空）
                            })});
        auto [req_ec, out] = co_await secs::utils::async_request_decoded(
            proto,
            2,  // Stream（流号）
            41, // Function（功能号）
            command);

        if (req_ec) {
            std::cout << "[客户端] S2F41 失败: " << req_ec.message() << "\n";
        } else {
            std::cout << "[客户端] 收到 S2F42 响应\n";
            (void)out;
        }
    }

    // 示例 3：单向消息（W=0，不等待响应）
    {
        std::cout << "\n[客户端] 发送 S6F11 (Event Report, 单向)...\n";

        ii::Item event =
            ii::Item::list({ii::Item::u4({1}),   // DATAID（数据 ID）
                            ii::Item::u4({100}), // CEID（采集事件 ID）
                            ii::Item::list({
                                // RPT（空报告）
                            })});

        ec = co_await secs::utils::async_send_item(
            proto,
            6,  // Stream（流号）
            11, // Function（功能号）
            event);
        if (ec) {
            std::cout << "[客户端] S6F11 发送失败: " << ec.message() << "\n";
        } else {
            std::cout << "[客户端] S6F11 已发送\n";
        }
    }

    std::cout << "\n[客户端] 示例完成，按 Ctrl+C 退出\n";

    // 保持连接：等待内部 reader_loop_ 退出（Ctrl+C 会触发 stop() 并唤醒）。
    (void)co_await session.async_wait_reader_stopped(std::nullopt);
}

int main(int argc, char *argv[]) {
    std::string host = "127.0.0.1";
    std::uint16_t port = 5000;

    if (argc > 1) {
        host = argv[1];
    }
    if (argc > 2) {
        port = static_cast<std::uint16_t>(std::atoi(argv[2]));
    }

    std::cout << "=== HSMS 客户端示例 ===\n\n";

    try {
        asio::io_context ioc;

        // 配置 HSMS 会话参数（示例值）
        hsms::SessionOptions opt;
        opt.session_id = 0x0001;
        opt.t3 = 45s; // T3：回复超时
        opt.t6 = 5s;  // T6：控制事务超时
        opt.t7 = 10s; // T7：未进入“已选择”状态的超时
        opt.t8 = 5s;  // T8：字符间隔超时
        opt.linktest_interval = 30s; // 定期发送 LINKTEST

        auto session = std::make_shared<hsms::Session>(ioc.get_executor(), opt);

        // protocol 层 Session：开启运行时报文 dump（调试用途）
        secs::protocol::SessionOptions proto_opt{};
        proto_opt.t3 = opt.t3;
        proto_opt.poll_interval = 20ms;
        proto_opt.dump.enable = true;
        proto_opt.dump.dump_tx = true;
        proto_opt.dump.dump_rx = true;
        proto_opt.dump.sink = dump_to_stdout;
        proto_opt.dump.sink_user = nullptr;
        proto_opt.dump.hsms.include_hex = false;
        proto_opt.dump.hsms.enable_secs2_decode = true;
        proto_opt.dump.hsms.item.max_payload_bytes = 256;

        auto proto = std::make_shared<secs::protocol::Session>(
            *session, opt.session_id, proto_opt);

        // 解析目标地址
        asio::ip::tcp::resolver resolver(ioc);
        auto endpoints = resolver.resolve(host, std::to_string(port));
        asio::ip::tcp::endpoint endpoint = *endpoints.begin();

        // 信号处理
        asio::signal_set signals(ioc, SIGINT, SIGTERM);
        signals.async_wait([&](const std::error_code &, int) {
            std::cout << "\n[客户端] 收到退出信号\n";
            proto->stop();
            session->stop();
            ioc.stop();
        });

        // 启动客户端会话
        asio::co_spawn(
            ioc,
            [&]() -> asio::awaitable<void> {
                co_await client_session(*session, *proto, endpoint);
                proto->stop();
                session->stop();
            },
            asio::detached);

        ioc.run();
    } catch (const std::exception &e) {
        std::cerr << "[客户端] 异常: " << e.what() << "\n";
        return 1;
    }

    std::cout << "[客户端] 已退出\n";
    return 0;
}
