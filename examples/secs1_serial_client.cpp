/**
 * @file secs1_serial_client.cpp
 * @brief SECS-I 示例（主机端）：通过串口/虚拟串口发送请求并等待回应
 *
 * 配合 `secs1_serial_server` 使用。推荐用虚拟串口（pty）测试，示例见：
 * `examples/secs1_serial_server.cpp` 的文件头说明。
 */

#include "secs1_serial_link_posix.hpp"

#include "secs/core/common.hpp"
#include "secs/protocol/session.hpp"
#include "secs/secs1/state_machine.hpp"

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/this_coro.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace secs;
using namespace std::chrono_literals;

namespace {

struct Options final {
    std::string tty_path;
    std::uint16_t device_id{1};
    int baud{9600};
    bool reverse_bit{false}; // 主机端：Host -> Equipment（R=0）
    std::size_t payload_bytes{700};
    std::uint32_t timeout_ms{5000};
};

static void print_usage(const char *prog) {
    std::cerr << "用法:\n";
    std::cerr << "  " << prog << " <tty_path> [options]\n\n";
    std::cerr << "选项:\n";
    std::cerr << "  --device-id <n>       设备 ID（默认 1）\n";
    std::cerr << "  --baud <n>            波特率（默认 9600；虚拟串口可忽略）\n";
    std::cerr << "  --reverse-bit <0|1>   R-bit 方向位（默认 0：主机端）\n";
    std::cerr << "  --payload-bytes <n>   payload 字节数（默认 700，触发分包）\n";
    std::cerr << "  --timeout-ms <n>      T3 超时（默认 5000ms）\n";
    std::cerr << "  -h, --help            显示帮助\n";
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

static bool parse_u32(const char *s, std::uint32_t &out) {
    if (!s) {
        return false;
    }
    char *end = nullptr;
    const unsigned long v = std::strtoul(s, &end, 10);
    if (!end || *end != '\0' || v > 0xFFFFFFFFUL) {
        return false;
    }
    out = static_cast<std::uint32_t>(v);
    return true;
}

static bool parse_size(const char *s, std::size_t &out) {
    if (!s) {
        return false;
    }
    char *end = nullptr;
    const unsigned long long v = std::strtoull(s, &end, 10);
    if (!end || *end != '\0') {
        return false;
    }
    out = static_cast<std::size_t>(v);
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
            int v = 0;
            const char *s = need_value("--baud");
            if (!s || !parse_int(s, v) || v <= 0) {
                std::cerr << "非法 baud: " << (s ? s : "") << "\n";
                return std::nullopt;
            }
            opt.baud = v;
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
        if (a == "--payload-bytes") {
            const char *s = need_value("--payload-bytes");
            if (!s || !parse_size(s, opt.payload_bytes) || opt.payload_bytes == 0) {
                std::cerr << "非法 payload-bytes: " << (s ? s : "") << "\n";
                return std::nullopt;
            }
            continue;
        }
        if (a == "--timeout-ms") {
            const char *s = need_value("--timeout-ms");
            if (!s || !parse_u32(s, opt.timeout_ms) || opt.timeout_ms == 0) {
                std::cerr << "非法 timeout-ms: " << (s ? s : "") << "\n";
                return std::nullopt;
            }
            continue;
        }

        std::cerr << "未知参数: " << a << "\n";
        return std::nullopt;
    }

    return opt;
}

static std::vector<core::byte> make_payload(std::size_t n) {
    std::vector<core::byte> out;
    out.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        out[i] = static_cast<core::byte>(i & 0xFFu);
    }
    return out;
}

asio::awaitable<int> run(const Options &opt, secs::examples::UniqueFd fd) {
    const auto ex = co_await asio::this_coro::executor;

    secs::examples::PosixSerialLink link(ex, std::move(fd));
    secs::secs1::StateMachine sm(link, opt.device_id);

    secs::protocol::SessionOptions sess_opt{};
    sess_opt.t3 = std::chrono::milliseconds(opt.timeout_ms);
    sess_opt.poll_interval = 20ms;
    sess_opt.secs1_reverse_bit = opt.reverse_bit;

    secs::protocol::Session sess(sm, opt.device_id, sess_opt);

    const auto payload = make_payload(opt.payload_bytes);
    std::cout << "[主机端] 发送: S1F13 (W=1), payload=" << payload.size()
              << " bytes\n";

    auto [ec, rsp] = co_await sess.async_request(
        1,
        13,
        core::bytes_view{payload.data(), payload.size()},
        std::chrono::milliseconds(opt.timeout_ms));

    if (ec) {
        std::cerr << "[主机端] 请求失败: " << ec.message() << "\n";
        sess.stop();
        co_return 1;
    }

    std::cout << "[主机端] 收到回应: S" << static_cast<int>(rsp.stream) << "F"
              << static_cast<int>(rsp.function) << " (W=" << rsp.w_bit
              << ", body=" << rsp.body.size() << " bytes)\n";

    if (rsp.stream != 1 || rsp.function != 14 || rsp.w_bit) {
        std::cerr << "[主机端] 响应头不符合预期\n";
        sess.stop();
        co_return 2;
    }
    if (rsp.body != payload) {
        std::cerr << "[主机端] 响应 payload 不一致\n";
        sess.stop();
        co_return 3;
    }

    std::cout << "PASS\n";
    sess.stop();
    co_return 0;
}

} // namespace

int main(int argc, char **argv) {
    const auto opt = parse_args(argc, argv);
    if (!opt.has_value()) {
        print_usage(argv[0]);
        return 2;
    }

    std::cout << "=== SECS-I Serial Client (Host) ===\n\n";
    std::cout << "[主机端] tty: " << opt->tty_path << "\n";
    std::cout << "[主机端] device_id: " << opt->device_id << "\n";
    std::cout << "[主机端] baud: " << opt->baud << "\n";
    std::cout << "[主机端] reverse_bit: " << (opt->reverse_bit ? 1 : 0)
              << "\n";
    std::cout << "[主机端] payload_bytes: " << opt->payload_bytes << "\n";
    std::cout << "[主机端] timeout_ms: " << opt->timeout_ms << "\n\n";

    auto [open_ec, fd] =
        secs::examples::open_tty_raw(opt->tty_path, opt->baud);
    if (open_ec) {
        std::cerr << "[主机端] 打开/配置 tty 失败: " << open_ec.message()
                  << "\n";
        return 1;
    }

    asio::io_context ioc;
    int rc = 1;
    asio::co_spawn(
        ioc,
        [&, fd = std::move(fd)]() mutable -> asio::awaitable<void> {
            rc = co_await run(*opt, std::move(fd));
            ioc.stop();
            co_return;
        },
        asio::detached);

    ioc.run();
    return rc;
}
