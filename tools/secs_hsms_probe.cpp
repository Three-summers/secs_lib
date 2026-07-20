/**
 * @file secs_hsms_probe.cpp
 * @brief HSMS Active Client 联调工具：连接设备、发送测试报文、查看收发内容。
 *
 * 典型场景：开发板作为 HSMS Passive Server 监听端口；本工具作为 Active Client
 * 用 IP 主动连接，SELECT 后发送/接收 DataMessage，并在终端打印解码结果。
 *
 * 用法示例：
 *   # 仅连接并持续观察（对端 W=1 主动消息默认回空 secondary）
 *   ./secs-hsms-probe --connect 192.168.1.50:5000 --session-id 0x0001
 *
 *   # 发送 S1F1(W=1) 空 body，等待 S1F2 后退出
 *   ./secs-hsms-probe --connect 192.168.1.50:5000 --send s1f1
 *
 *   # 发送自定义 body，W=0 单向发送后保持连接观察
 *   ./secs-hsms-probe --connect 192.168.1.50:5000 --send s6f11 --w 0 \
 *       --body-hex "01 03 00 01 00 00 00 64 00" --hold
 *
 *   # 连接后先 LINKTEST，再发请求，并录制 JSONL
 *   ./secs-hsms-probe --connect 192.168.1.50:5000 --linktest --send s1f1 \
 *       --record /tmp/probe.jsonl
 */

#include "secs/core/common.hpp"
#include "secs/core/error.hpp"
#include "secs/hsms/session.hpp"
#include "secs/protocol/session.hpp"
#include "secs/tools/recording.hpp"
#include "secs/utils/hex.hpp"
#include "secs/utils/protocol_helpers.hpp"

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/signal_set.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace {

struct Options final {
    std::string connect;
    std::uint16_t session_id{0x0001};

    // 可选：连接成功后发送一条 DataMessage。
    bool has_send{false};
    std::uint8_t send_stream{0};
    std::uint8_t send_function{0};
    bool send_w_bit{true};
    std::vector<secs::core::byte> body{};

    // 请求等待超时（W=1 时用于 async_request）。
    secs::core::duration timeout{std::chrono::seconds{5}};

    // HSMS 定时器（仅暴露常用项）。
    secs::core::duration t3{std::chrono::seconds{45}};
    secs::core::duration t6{std::chrono::seconds{5}};
    secs::core::duration t7{std::chrono::seconds{10}};
    secs::core::duration t8{std::chrono::seconds{5}};
    secs::core::duration linktest_interval{0s};

    bool do_linktest{false};
    bool hold{false}; // 发送完成后保持连接；无 --send 时默认 hold

    // 入站 primary（W=1）自动回 secondary body 策略。
    enum class AutoReply : std::uint8_t { empty = 0, none = 1 };
    AutoReply auto_reply{AutoReply::empty};

    // dump 选项
    bool dump{true};
    bool dump_hex{false};
    bool dump_secs2{true};
    bool color{false};
    std::size_t max_payload_bytes{256};

    // 可选 JSONL 录制（protocol tap）
    std::string record_path;
    bool record_sml_header{true};
};

static void print_usage(const char *argv0) {
    std::cout
        << "用法:\n"
        << "  " << argv0
        << " --connect <ip:port> [options]\n\n"
        << "说明:\n"
        << "  - 作为 HSMS Active Client 连接设备（Passive Server）\n"
        << "  - SELECT 成功后可选发送一条 DataMessage，并打印收发内容\n"
        << "  - 无 --send 时默认保持连接，观察对端主动上报\n"
        << "  - 对端 primary 且 W=1 时，默认自动回空 body secondary\n\n"
        << "选项:\n"
        << "  --connect <ip:port>     目标地址（例如 192.168.1.50:5000）\n"
        << "  --session-id <u16>      HSMS data SessionID（默认 0x0001，支持 0x 前缀）\n"
        << "  --send <SxFy|s=N,f=M>   发送主消息（例如 s1f1 / S6F11 / s=1,f=1）\n"
        << "  --body-hex <hex>        发送 body（SECS-II 编码后的字节，默认空）\n"
        << "  --w <0|1>               发送 Wait bit（默认 1；W=1 时等待 secondary）\n"
        << "  --timeout-ms <u32>      等待 reply 超时毫秒（默认 5000）\n"
        << "  --t3-ms <u32>           协议层 T3 毫秒（默认 45000）\n"
        << "  --linktest              SELECT 后先做一次 LINKTEST\n"
        << "  --hold                  发送完成后保持连接（无 --send 时默认保持）\n"
        << "  --auto-reply <empty|none>\n"
        << "                          入站 primary(W=1) 自动回包策略（默认 empty）\n"
        << "  --record <file.jsonl>   录制 DataMessage 为 JSONL\n"
        << "  --no-dump               关闭 protocol dump 输出\n"
        << "  --hex                   dump 中包含 body hexdump\n"
        << "  --no-secs2              dump 中不解码 SECS-II Item\n"
        << "  --color                 dump 启用 ANSI 颜色\n"
        << "  --max-payload <N>       SECS-II dump 最大 payload 字节（默认 256）\n"
        << "  -h, --help              显示帮助\n\n"
        << "示例:\n"
        << "  " << argv0 << " --connect 192.168.1.50:5000\n"
        << "  " << argv0 << " --connect 192.168.1.50:5000 --send s1f1\n"
        << "  " << argv0
        << " --connect 192.168.1.50:5000 --send s2f41 --body-hex \"01 02 ...\" --hold\n";
}

static bool parse_u16(std::string_view s, std::uint16_t &out) {
    unsigned v = 0;
    int base = 10;
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s.remove_prefix(2);
    }
    if (s.empty()) {
        return false;
    }
    const auto *begin = s.data();
    const auto *end = s.data() + s.size();
    const auto [ptr, ec] = std::from_chars(begin, end, v, base);
    if (ec != std::errc{} || ptr != end ||
        v > static_cast<unsigned>(std::numeric_limits<std::uint16_t>::max())) {
        return false;
    }
    out = static_cast<std::uint16_t>(v);
    return true;
}

static bool parse_u32(std::string_view s, std::uint32_t &out) {
    unsigned long long v = 0;
    int base = 10;
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s.remove_prefix(2);
    }
    if (s.empty()) {
        return false;
    }
    const auto *begin = s.data();
    const auto *end = s.data() + s.size();
    const auto [ptr, ec] = std::from_chars(begin, end, v, base);
    if (ec != std::errc{} || ptr != end ||
        v > static_cast<unsigned long long>(
                std::numeric_limits<std::uint32_t>::max())) {
        return false;
    }
    out = static_cast<std::uint32_t>(v);
    return true;
}

static bool parse_u8(std::string_view s, std::uint8_t &out) {
    unsigned v = 0;
    if (s.empty()) {
        return false;
    }
    const auto *begin = s.data();
    const auto *end = s.data() + s.size();
    const auto [ptr, ec] = std::from_chars(begin, end, v, 10);
    if (ec != std::errc{} || ptr != end || v > 255U) {
        return false;
    }
    out = static_cast<std::uint8_t>(v);
    return true;
}

static bool parse_size_t(std::string_view s, std::size_t &out) {
    unsigned long long v = 0;
    if (s.empty()) {
        return false;
    }
    const auto *begin = s.data();
    const auto *end = s.data() + s.size();
    const auto [ptr, ec] = std::from_chars(begin, end, v, 10);
    if (ec != std::errc{} || ptr != end) {
        return false;
    }
    out = static_cast<std::size_t>(v);
    return true;
}

static bool parse_endpoint(std::string_view text, asio::ip::tcp::endpoint &out) {
    std::string host;
    std::string_view port_sv;

    if (!text.empty() && text.front() == '[') {
        const auto rb = text.find(']');
        if (rb == std::string_view::npos || rb + 2 > text.size() ||
            text[rb + 1] != ':') {
            return false;
        }
        host = std::string(text.substr(1, rb - 1));
        port_sv = text.substr(rb + 2);
    } else {
        const auto colon = text.rfind(':');
        if (colon == std::string_view::npos) {
            return false;
        }
        host = std::string(text.substr(0, colon));
        port_sv = text.substr(colon + 1);
    }

    std::uint16_t port = 0;
    if (!parse_u16(port_sv, port)) {
        return false;
    }

    std::error_code ec{};
    const auto addr = asio::ip::make_address(host, ec);
    if (ec) {
        return false;
    }
    out = asio::ip::tcp::endpoint(addr, port);
    return true;
}

// 解析 "s1f1" / "S1F1" / "s=1,f=1"
static bool parse_sf(std::string_view s,
                     std::uint8_t &stream,
                     std::uint8_t &function) {
    if (s.empty()) {
        return false;
    }

    // s=N,f=M
    if (s.find('=') != std::string_view::npos) {
        std::optional<std::uint8_t> st;
        std::optional<std::uint8_t> fn;
        while (!s.empty()) {
            const auto comma = s.find(',');
            const auto part =
                (comma == std::string_view::npos) ? s : s.substr(0, comma);
            const auto eq = part.find('=');
            if (eq == std::string_view::npos) {
                return false;
            }
            const auto key = part.substr(0, eq);
            const auto val = part.substr(eq + 1);
            std::uint8_t v = 0;
            if (!parse_u8(val, v)) {
                return false;
            }
            if (key == "s" || key == "S") {
                st = v;
            } else if (key == "f" || key == "F") {
                fn = v;
            } else {
                return false;
            }
            if (comma == std::string_view::npos) {
                break;
            }
            s.remove_prefix(comma + 1);
        }
        if (!st.has_value() || !fn.has_value()) {
            return false;
        }
        stream = *st;
        function = *fn;
        return true;
    }

    // SxFy / s1f1
    std::size_t i = 0;
    if (i < s.size() && (s[i] == 's' || s[i] == 'S')) {
        ++i;
    } else {
        return false;
    }

    std::size_t stream_begin = i;
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
        ++i;
    }
    if (stream_begin == i || i >= s.size() || (s[i] != 'f' && s[i] != 'F')) {
        return false;
    }
    const auto stream_sv = s.substr(stream_begin, i - stream_begin);
    ++i; // skip f/F
    std::size_t func_begin = i;
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
        ++i;
    }
    if (func_begin == i || i != s.size()) {
        return false;
    }
    const auto func_sv = s.substr(func_begin, i - func_begin);

    std::uint8_t st = 0;
    std::uint8_t fn = 0;
    if (!parse_u8(stream_sv, st) || !parse_u8(func_sv, fn)) {
        return false;
    }
    stream = st;
    function = fn;
    return true;
}

static bool parse_auto_reply(std::string_view s, Options::AutoReply &out) {
    if (s == "empty") {
        out = Options::AutoReply::empty;
        return true;
    }
    if (s == "none") {
        out = Options::AutoReply::none;
        return true;
    }
    return false;
}

static std::optional<Options> parse_args(int argc, char **argv) {
    Options opt{};
    for (int i = 1; i < argc; ++i) {
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
        if (a == "--connect") {
            const char *v = need_value("--connect");
            if (!v) {
                return std::nullopt;
            }
            opt.connect = v;
            continue;
        }
        if (a == "--session-id") {
            const char *v = need_value("--session-id");
            if (!v || !parse_u16(v, opt.session_id)) {
                std::cerr << "非法 session-id: " << (v ? v : "") << "\n";
                return std::nullopt;
            }
            continue;
        }
        if (a == "--send") {
            const char *v = need_value("--send");
            if (!v || !parse_sf(v, opt.send_stream, opt.send_function)) {
                std::cerr << "非法 --send: " << (v ? v : "")
                          << "（期望 s1f1 / S6F11 / s=1,f=1）\n";
                return std::nullopt;
            }
            opt.has_send = true;
            continue;
        }
        if (a == "--body-hex") {
            const char *v = need_value("--body-hex");
            if (!v) {
                return std::nullopt;
            }
            std::vector<secs::core::byte> body;
            const auto ec = secs::utils::parse_hex(v, body);
            if (ec) {
                std::cerr << "非法 --body-hex: " << ec.message() << "\n";
                return std::nullopt;
            }
            opt.body = std::move(body);
            continue;
        }
        if (a == "--w") {
            const char *v = need_value("--w");
            std::uint8_t w = 0;
            if (!v || !parse_u8(v, w) || (w != 0 && w != 1)) {
                std::cerr << "非法 --w: " << (v ? v : "") << "（期望 0|1）\n";
                return std::nullopt;
            }
            opt.send_w_bit = (w == 1);
            continue;
        }
        if (a == "--timeout-ms") {
            const char *v = need_value("--timeout-ms");
            std::uint32_t ms = 0;
            if (!v || !parse_u32(v, ms)) {
                std::cerr << "非法 --timeout-ms: " << (v ? v : "") << "\n";
                return std::nullopt;
            }
            opt.timeout = std::chrono::milliseconds{ms};
            continue;
        }
        if (a == "--t3-ms") {
            const char *v = need_value("--t3-ms");
            std::uint32_t ms = 0;
            if (!v || !parse_u32(v, ms)) {
                std::cerr << "非法 --t3-ms: " << (v ? v : "") << "\n";
                return std::nullopt;
            }
            opt.t3 = std::chrono::milliseconds{ms};
            continue;
        }
        if (a == "--linktest") {
            opt.do_linktest = true;
            continue;
        }
        if (a == "--hold") {
            opt.hold = true;
            continue;
        }
        if (a == "--auto-reply") {
            const char *v = need_value("--auto-reply");
            if (!v || !parse_auto_reply(v, opt.auto_reply)) {
                std::cerr << "非法 --auto-reply: " << (v ? v : "")
                          << "（期望 empty|none）\n";
                return std::nullopt;
            }
            continue;
        }
        if (a == "--record") {
            const char *v = need_value("--record");
            if (!v) {
                return std::nullopt;
            }
            opt.record_path = v;
            continue;
        }
        if (a == "--no-dump") {
            opt.dump = false;
            continue;
        }
        if (a == "--hex") {
            opt.dump_hex = true;
            continue;
        }
        if (a == "--no-secs2") {
            opt.dump_secs2 = false;
            continue;
        }
        if (a == "--color") {
            opt.color = true;
            continue;
        }
        if (a == "--max-payload") {
            const char *v = need_value("--max-payload");
            if (!v || !parse_size_t(v, opt.max_payload_bytes)) {
                std::cerr << "非法 --max-payload: " << (v ? v : "") << "\n";
                return std::nullopt;
            }
            continue;
        }

        std::cerr << "未知参数: " << a << "\n";
        return std::nullopt;
    }

    if (opt.connect.empty()) {
        std::cerr << "缺少 --connect\n";
        return std::nullopt;
    }
    // 未指定 --send 时默认保持连接观察；指定 --send 默认发完退出，除非 --hold。
    if (!opt.has_send) {
        opt.hold = true;
    }
    return opt;
}

static void dump_to_stdout(void *,
                           const char *data,
                           std::size_t size) noexcept {
    if (!data || size == 0) {
        return;
    }
    (void)std::fwrite(data, 1, size, stdout);
    (void)std::fflush(stdout);
}

static void on_control_event(void *,
                             const secs::hsms::ControlEvent &ev) noexcept {
    const char *dir = (ev.direction == secs::hsms::ControlDirection::tx) ? "TX"
                                                                         : "RX";
    const char *stype = "CTRL";
    switch (ev.s_type) {
    case secs::hsms::SType::select_req:
        stype = "SELECT.req";
        break;
    case secs::hsms::SType::select_rsp:
        stype = "SELECT.rsp";
        break;
    case secs::hsms::SType::deselect_req:
        stype = "DESELECT.req";
        break;
    case secs::hsms::SType::deselect_rsp:
        stype = "DESELECT.rsp";
        break;
    case secs::hsms::SType::linktest_req:
        stype = "LINKTEST.req";
        break;
    case secs::hsms::SType::linktest_rsp:
        stype = "LINKTEST.rsp";
        break;
    case secs::hsms::SType::reject_req:
        stype = "REJECT.req";
        break;
    case secs::hsms::SType::separate_req:
        stype = "SEPARATE.req";
        break;
    case secs::hsms::SType::data:
        stype = "DATA";
        break;
    default:
        stype = "CTRL";
        break;
    }

    std::printf("[ctrl][%s] %s session=0x%04x sb=%u hb2=%u hb3=%u\n",
                dir,
                stype,
                static_cast<unsigned>(ev.session_id),
                static_cast<unsigned>(ev.system_bytes),
                static_cast<unsigned>(ev.header_byte2),
                static_cast<unsigned>(ev.header_byte3));
    std::fflush(stdout);
}

static void print_msg_summary(const char *tag,
                              const secs::protocol::DataMessage &msg) {
    std::cout << tag << " S" << static_cast<int>(msg.stream) << "F"
              << static_cast<int>(msg.function) << " W=" << (msg.w_bit ? 1 : 0)
              << " sb=" << msg.system_bytes << " body_n=" << msg.body.size()
              << "\n";
}

static asio::awaitable<int> run(const Options &opt) {
    auto ex = co_await asio::this_coro::executor;

    asio::ip::tcp::endpoint ep;
    if (!parse_endpoint(opt.connect, ep)) {
        std::cout << "非法 connect: " << opt.connect << "\n";
        co_return 2;
    }

    std::unique_ptr<secs::tools::MessageRecorder> recorder;
    if (!opt.record_path.empty()) {
        recorder = std::make_unique<secs::tools::MessageRecorder>(opt.record_path);
        if (!recorder->is_open()) {
            std::cout << "打开录制文件失败: " << opt.record_path
                      << " ec=" << recorder->last_error().message() << "\n";
            co_return 2;
        }
        recorder->set_include_sml_header(opt.record_sml_header);
    }

    secs::hsms::SessionOptions hsms_opt{};
    hsms_opt.session_id = opt.session_id;
    hsms_opt.t3 = opt.t3;
    hsms_opt.t5 = 1s;
    hsms_opt.t6 = opt.t6;
    hsms_opt.t7 = opt.t7;
    hsms_opt.t8 = opt.t8;
    hsms_opt.linktest_interval = opt.linktest_interval;
    hsms_opt.auto_reconnect = false;
    hsms_opt.passive_accept_select = true;
    hsms_opt.on_control_event = &on_control_event;
    hsms_opt.on_control_event_user = nullptr;

    auto hsms = std::make_shared<secs::hsms::Session>(ex, hsms_opt);

    secs::protocol::SessionOptions proto_opt{};
    proto_opt.t3 = opt.t3;
    proto_opt.poll_interval = 10ms;

    if (opt.dump) {
        proto_opt.dump.enable = true;
        proto_opt.dump.dump_tx = true;
        proto_opt.dump.dump_rx = true;
        proto_opt.dump.sink = &dump_to_stdout;
        proto_opt.dump.sink_user = nullptr;
        proto_opt.dump.hsms.include_hex = opt.dump_hex;
        proto_opt.dump.hsms.enable_color = opt.color;
        proto_opt.dump.hsms.enable_secs2_decode = opt.dump_secs2;
        proto_opt.dump.hsms.item.max_payload_bytes = opt.max_payload_bytes;
        proto_opt.dump.hsms.item.enable_color = opt.color;
        if (opt.dump_hex) {
            proto_opt.dump.hsms.hex.max_bytes = 0; // 不截断 hex（body 可能更大）
            proto_opt.dump.hsms.hex.enable_color = opt.color;
        }
    }

    if (recorder) {
        proto_opt.tap.enable = true;
        proto_opt.tap.tap_tx = true;
        proto_opt.tap.tap_rx = true;
        proto_opt.tap.on_message_user = recorder.get();
        proto_opt.tap.on_message =
            [](void *user, const secs::protocol::DataMessage &msg,
               bool is_tx) noexcept {
                auto *rec = static_cast<secs::tools::MessageRecorder *>(user);
                if (!rec) {
                    return;
                }
                if (is_tx) {
                    rec->record_tx(msg);
                } else {
                    rec->record_rx(msg);
                }
            };
    }

    auto proto = std::make_shared<secs::protocol::Session>(
        *hsms, opt.session_id, proto_opt);

    if (opt.auto_reply == Options::AutoReply::empty) {
        // 对任意未精确匹配的入站 primary：若 W=1，protocol 层会自动回 secondary，
        // body 使用 handler 返回值（此处为空 body）。
        proto->router().set_default(
            [](const secs::protocol::DataMessage &msg)
                -> asio::awaitable<secs::protocol::HandlerResult> {
                std::cout << "[auto-reply] inbound primary S"
                          << static_cast<int>(msg.stream) << "F"
                          << static_cast<int>(msg.function)
                          << " W=" << (msg.w_bit ? 1 : 0)
                          << " sb=" << msg.system_bytes
                          << " body_n=" << msg.body.size()
                          << (msg.w_bit ? " -> empty secondary\n"
                                        : " (no reply, W=0)\n");
                co_return secs::protocol::HandlerResult{std::error_code{},
                                                        {}};
            });
    }

    std::cout << "=== secs-hsms-probe ===\n";
    std::cout << "[probe] connect: " << ep << " session_id=0x" << std::hex
              << opt.session_id << std::dec << "\n";

    const auto open_ec = co_await hsms->async_open_active(ep);
    if (open_ec) {
        std::cout << "[probe] open/SELECT failed: " << open_ec.message() << "\n";
        co_return 1;
    }
    std::cout << "[probe] selected\n";

    // 信号处理：在 run 协程内注册，确保能 stop 会话。
    asio::signal_set signals(ex, SIGINT, SIGTERM);
    signals.async_wait([proto, hsms](const std::error_code &, int) {
        std::cout << "\n[probe] 收到退出信号\n";
        proto->stop();
        hsms->stop();
    });

    if (opt.do_linktest) {
        std::cout << "[probe] LINKTEST...\n";
        const auto lt_ec = co_await hsms->async_linktest();
        if (lt_ec) {
            std::cout << "[probe] LINKTEST failed: " << lt_ec.message() << "\n";
            proto->stop();
            hsms->stop();
            (void)co_await hsms->async_wait_reader_stopped(1s);
            co_return 1;
        }
        std::cout << "[probe] LINKTEST ok\n";
    }

    int rc = 0;

    if (opt.has_send) {
        const secs::core::bytes_view body{opt.body.data(), opt.body.size()};
        std::cout << "[probe] send S" << static_cast<int>(opt.send_stream)
                  << "F" << static_cast<int>(opt.send_function)
                  << " W=" << (opt.send_w_bit ? 1 : 0)
                  << " body_n=" << opt.body.size() << "\n";

        if (opt.send_w_bit) {
            auto [ec, reply] = co_await proto->async_request(
                opt.send_stream, opt.send_function, body, opt.timeout);
            if (ec) {
                std::cout << "[probe] request failed: " << ec.message() << "\n";
                // 若已收到 reply 但后续处理失败，仍尽量打印。
                if (reply.function != 0 || !reply.body.empty()) {
                    print_msg_summary("[probe] partial reply", reply);
                }
                rc = 1;
            } else {
                print_msg_summary("[probe] reply", reply);
            }
        } else {
            const auto ec =
                co_await proto->async_send(opt.send_stream, opt.send_function, body);
            if (ec) {
                std::cout << "[probe] send failed: " << ec.message() << "\n";
                rc = 1;
            } else {
                std::cout << "[probe] send ok\n";
            }
        }
    }

    if (opt.hold) {
        // 说明：
        // - async_request 会在 HSMS 后端自动启动 protocol 接收循环；
        // - 若再次 co_await async_run()，内部可能因 run_loop_active_ 立即返回；
        // - 因此 hold 时始终 detached 启动 async_run（幂等），再等待底层 reader 退出。
        if (rc == 0) {
            std::cout << "[probe] holding connection (Ctrl+C to exit)...\n";
        } else {
            std::cout << "[probe] hold after error (Ctrl+C to exit)...\n";
        }
        asio::co_spawn(
            ex,
            [proto]() -> asio::awaitable<void> { co_await proto->async_run(); },
            asio::detached);
        (void)co_await hsms->async_wait_reader_stopped(std::nullopt);
    }

    if (recorder) {
        recorder->flush();
    }

    proto->stop();
    hsms->stop();
    (void)co_await hsms->async_wait_reader_stopped(1s);
    std::cout << "[probe] done rc=" << rc << "\n";
    co_return rc;
}

} // namespace

int main(int argc, char **argv) {
    const auto opt = parse_args(argc, argv);
    if (!opt.has_value()) {
        print_usage(argv[0]);
        return 2;
    }

    asio::io_context ioc;
    int rc = 1;

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            rc = co_await run(*opt);
            ioc.stop();
        },
        asio::detached);

    ioc.run();
    return rc;
}
