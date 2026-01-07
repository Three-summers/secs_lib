/**
 * @file hsms_server.cpp
 * @brief HSMS 服务器示例 - 监听连接并处理 SECS-II 消息
 *
 * 用法: ./hsms_server [port]
 * 默认端口: 5000
 */

#include <secs/hsms/session.hpp>
#include <secs/ii/codec.hpp>
#include <secs/ii/item.hpp>
#include <secs/protocol/session.hpp>

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

asio::awaitable<void> handle_session(hsms::Session &,
                                     protocol::Session &proto) {
    std::cout << "[服务器] 会话已建立，等待数据消息...\n";

    // default handler：打印消息，并在 W=1 时自动回 "OK"
    proto.router().set_default(
        [](const protocol::DataMessage &msg)
            -> asio::awaitable<protocol::HandlerResult> {
            std::cout << "[服务器] 收到消息: S" << static_cast<int>(msg.stream)
                      << "F" << static_cast<int>(msg.function)
                      << " (W=" << (msg.w_bit ? 1 : 0) << ")\n";

            // 解码 SECS-II 数据（仅演示：失败不影响回包）
            if (!msg.body.empty()) {
                ii::Item decoded{ii::Item::ascii("")};
                std::size_t consumed = 0;
                auto dec_ec = ii::decode_one(
                    core::bytes_view{msg.body.data(), msg.body.size()},
                    decoded,
                    consumed);

                if (!dec_ec) {
                    if (auto *ascii = decoded.get_if<ii::ASCII>()) {
                        std::cout << "[服务器] 数据内容 (ASCII): \"" << ascii->value
                                  << "\"\n";
                    } else if (auto *list = decoded.get_if<ii::List>()) {
                        std::cout << "[服务器] 数据内容 (List): " << list->size()
                                  << " 项\n";
                    } else {
                        std::cout << "[服务器] 数据内容: " << msg.body.size()
                                  << " 字节\n";
                    }
                }
            }

            // 构造响应：S{n}F{n+1}
            ii::Item reply_item = ii::Item::ascii("OK");
            std::vector<core::byte> reply_body;
            ii::encode(reply_item, reply_body);

            co_return protocol::HandlerResult{std::error_code{},
                                              std::move(reply_body)};
        });

    co_await proto.async_run();

    std::cout << "[服务器] 会话结束\n";
}

asio::awaitable<void> server_loop(asio::ip::tcp::acceptor &acceptor,
                                  hsms::SessionOptions &opt) {
    std::cout << "[服务器] 等待客户端连接...\n";

    while (true) {
        auto [ec, socket] =
            co_await acceptor.async_accept(asio::as_tuple(asio::use_awaitable));
        if (ec) {
            if (ec == asio::error::operation_aborted) {
                break;
            }
            std::cerr << "[服务器] Accept 错误: " << ec.message() << "\n";
            continue;
        }

        auto remote = socket.remote_endpoint();
        std::cout << "[服务器] 新连接: " << remote.address() << ":"
                  << remote.port() << "\n";

        // 每个连接创建新的 Session
        auto session =
            std::make_shared<hsms::Session>(acceptor.get_executor(), opt);

        asio::co_spawn(
            acceptor.get_executor(),
            [session,
             session_id = opt.session_id,
             s = std::move(socket)]() mutable -> asio::awaitable<void> {
                auto ec = co_await session->async_open_passive(std::move(s));
                if (ec) {
                    std::cout << "[服务器] SELECT 失败: " << ec.message()
                              << "\n";
                    co_return;
                }
                secs::protocol::SessionOptions proto_opt{};
                proto_opt.t3 = 45s;
                proto_opt.poll_interval = 20ms;
                proto_opt.dump.enable = true;
                proto_opt.dump.dump_tx = true;
                proto_opt.dump.dump_rx = true;
                proto_opt.dump.sink = dump_to_stdout;
                proto_opt.dump.sink_user = nullptr;
                proto_opt.dump.hsms.include_hex = false;
                proto_opt.dump.hsms.enable_secs2_decode = true;
                proto_opt.dump.hsms.item.max_payload_bytes = 256;

                secs::protocol::Session proto(*session,
                                              session_id,
                                              proto_opt);

                co_await handle_session(*session, proto);
                proto.stop();
                session->stop();
            },
            asio::detached);
    }
}

int main(int argc, char *argv[]) {
    std::uint16_t port = 5000;
    if (argc > 1) {
        port = static_cast<std::uint16_t>(std::atoi(argv[1]));
    }

    std::cout << "=== HSMS 服务器示例 ===\n\n";

    try {
        asio::io_context ioc;

        // 配置 HSMS 会话参数（示例值）
        hsms::SessionOptions opt;
        opt.session_id = 0x0001;
        opt.t3 = 45s; // T3：回复超时
        opt.t6 = 5s;  // T6：控制事务超时
        opt.t7 = 10s; // T7：未进入“已选择”状态的超时
        opt.t8 = 5s;  // T8：字符间隔超时

        // 创建 TCP 监听器
        asio::ip::tcp::acceptor acceptor(
            ioc, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), port));

        std::cout << "[服务器] 监听端口: " << port << "\n";

        // 信号处理
        asio::signal_set signals(ioc, SIGINT, SIGTERM);
        signals.async_wait([&](const std::error_code &, int) {
            std::cout << "\n[服务器] 收到退出信号\n";
            acceptor.close();
            ioc.stop();
        });

        // 启动服务器循环
        asio::co_spawn(ioc, server_loop(acceptor, opt), asio::detached);

        ioc.run();
    } catch (const std::exception &e) {
        std::cerr << "[服务器] 异常: " << e.what() << "\n";
        return 1;
    }

    std::cout << "[服务器] 已退出\n";
    return 0;
}
