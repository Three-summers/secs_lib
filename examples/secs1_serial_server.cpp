/**
 * @file secs1_serial_server.cpp
 * @brief SECS-I 示例（设备端）：通过串口/虚拟串口接收并回包
 *
 * 推荐用“虚拟串口（pty）”测试：
 *
 * 1) 创建一对互联的 pty（两端路径）：
 *    socat -d -d pty,raw,echo=0,link=/tmp/secs1_a pty,raw,echo=0,link=/tmp/secs1_b
 *
 * 2) 在两个终端分别启动：
 *    ./secs1_serial_server /tmp/secs1_b
 *    ./secs1_serial_client /tmp/secs1_a
 *
 * 本程序默认注册 S1F13 的 handler：回显 body，并自动发送 S1F14（W=0）。
 */

#include "secs1_serial_link_posix.hpp"

#include "secs/protocol/session.hpp"
#include "secs/secs1/state_machine.hpp"

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/signal_set.hpp>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

using namespace secs;
using namespace std::chrono_literals;

namespace {

struct Options final {
    std::string tty_path;
    std::uint16_t device_id{1};
    int baud{9600};
    bool reverse_bit{true}; // 设备端：Equipment -> Host（R=1）
};

static void print_usage(const char *prog) {
    std::cerr << "用法:\n";
    std::cerr << "  " << prog << " <tty_path> [options]\n\n";
    std::cerr << "选项:\n";
    std::cerr << "  --device-id <n>     设备 ID（默认 1）\n";
    std::cerr << "  --baud <n>          波特率（默认 9600；虚拟串口可忽略）\n";
    std::cerr << "  --reverse-bit <0|1> R-bit 方向位（默认 1：设备端）\n";
    std::cerr << "  -h, --help          显示帮助\n";
}

static bool parse_u16(const char *s, std::uint16_t &out) {
    if (!s) {
        return false;
    }
    char *end = nullptr;
    const unsigned long v = std::strtoul(s, &end, 10);
    if (!end || *end != '\0' || v > 65535UL) {
        return false;
    }
    out = static_cast<std::uint16_t>(v);
    return true;
}

static bool parse_int(const char *s, int &out) {
    if (!s) {
        return false;
    }
    char *end = nullptr;
    const long v = std::strtol(s, &end, 10);
    if (!end || *end != '\0') {
        return false;
    }
    out = static_cast<int>(v);
    return true;
}

static std::optional<Options> parse_args(int argc, char **argv) {
    if (argc < 2) {
        return std::nullopt;
    }

    Options opt;
    opt.tty_path = argv[1];

    for (int i = 2; i < argc; ++i) {
        const std::string_view a = argv[i];

        auto need_value = [&](const char *name) -> const char * {
            if (i + 1 >= argc) {
                std::cerr << "缺少参数值: " << name << "\n";
                return nullptr;
            }
            ++i;
            return argv[i];
        };

        if (a == "-h" || a == "--help") {
            return std::nullopt;
        }
        if (a == "--device-id") {
            const char *v = need_value("--device-id");
            if (!v || !parse_u16(v, opt.device_id)) {
                std::cerr << "非法 device-id: " << (v ? v : "") << "\n";
                return std::nullopt;
            }
            continue;
        }
        if (a == "--baud") {
            const char *v = need_value("--baud");
            if (!v || !parse_int(v, opt.baud) || opt.baud <= 0) {
                std::cerr << "非法 baud: " << (v ? v : "") << "\n";
                return std::nullopt;
            }
            continue;
        }
        if (a == "--reverse-bit") {
            int v = 0;
            const char *s = need_value("--reverse-bit");
            if (!s || !parse_int(s, v) || (v != 0 && v != 1)) {
                std::cerr << "非法 reverse-bit: " << (s ? s : "") << "\n";
                return std::nullopt;
            }
            opt.reverse_bit = (v != 0);
            continue;
        }

        std::cerr << "未知参数: " << a << "\n";
        return std::nullopt;
    }

    return opt;
}

} // namespace

int main(int argc, char **argv) {
    const auto opt = parse_args(argc, argv);
    if (!opt.has_value()) {
        print_usage(argv[0]);
        return 2;
    }

    std::cout << "=== SECS-I Serial Server (Equipment) ===\n\n";
    std::cout << "[设备端] tty: " << opt->tty_path << "\n";
    std::cout << "[设备端] device_id: " << opt->device_id << "\n";
    std::cout << "[设备端] baud: " << opt->baud << "\n";
    std::cout << "[设备端] reverse_bit: " << (opt->reverse_bit ? 1 : 0)
              << "\n\n";

    asio::io_context ioc;

    auto [open_ec, fd] =
        secs::examples::open_tty_raw(opt->tty_path, opt->baud);
    if (open_ec) {
        std::cerr << "[设备端] 打开/配置 tty 失败: " << open_ec.message()
                  << "\n";
        return 1;
    }

    secs::examples::PosixSerialLink link(ioc.get_executor(), std::move(fd));

    secs::secs1::StateMachine sm(link, opt->device_id);

    secs::protocol::SessionOptions sess_opt{};
    sess_opt.t3 = 5s;
    sess_opt.poll_interval = 20ms;
    sess_opt.secs1_reverse_bit = opt->reverse_bit;

    secs::protocol::Session sess(sm, opt->device_id, sess_opt);

    // 示例：回显 S1F13 的 body（自动回 S1F14）。
    sess.router().set(
        1,
        13,
        [](const secs::protocol::DataMessage &msg)
            -> asio::awaitable<secs::protocol::HandlerResult> {
            std::cout << "[设备端] 收到: S" << static_cast<int>(msg.stream)
                      << "F" << static_cast<int>(msg.function)
                      << " (W=" << (msg.w_bit ? 1 : 0) << ", system_bytes="
                      << msg.system_bytes << ", body=" << msg.body.size()
                      << " bytes)\n";
            co_return secs::protocol::HandlerResult{std::error_code{}, msg.body};
        });

    asio::signal_set signals(ioc, SIGINT, SIGTERM);
    signals.async_wait([&](const std::error_code &, int) {
        std::cout << "\n[设备端] 收到退出信号\n";
        sess.stop();
        ioc.stop();
    });

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            co_await sess.async_run();
        },
        asio::detached);

    std::cout << "[设备端] 就绪，等待请求（Ctrl+C 退出）...\n";
    ioc.run();

    std::cout << "[设备端] 已退出\n";
    return 0;
}
