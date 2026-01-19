/**
 * @file secs1_sml_peer.cpp
 * @brief SECS-I（串口）对端测试工具：加载同一份 SML，并按 SML 规则自动回包
 *
 * 目标场景：
 * - Windows 上使用 com0com 创建一对虚拟串口（例如 COM5 <-> COM6）；
 * - 被测程序可作为 Host 或 Equipment 任意一端；
 * - 本示例作为“对端”运行：打开其中一个串口，加载同一份 SML，按规则自动回包；
 * - 可选：启用 SML 的 every N send 规则周期性发送消息（用于联调时自动出流量）；
 *
 * 用法：
 *   ./secs1_sml_peer --help
 *
 * 示例（Windows/com0com）：
 *   # 作为 Equipment，对端（被测 Host）打开 COM6
 *   ./secs1_sml_peer --role equipment --serial COM5 --baud 9600 --device-id 0x0001 --sml docs/sml_sample/sample.sml
 *
 *   # 作为 Host，对端（被测 Equipment）打开 COM5
 *   ./secs1_sml_peer --role host --serial COM6 --baud 9600 --device-id 0x0001 --sml docs/sml_sample/sample.sml --enable-timers
 *
 * 注意：
 * - SECS-I 是半双工，本示例刻意避免并发收发：使用 protocol::Session::async_poll_once()
 *   在主循环里“单步收包并处理”，并在空闲时串行执行 fire/timer 发送。
 */

#include "secs/core/log.hpp"
#include "secs/protocol/session.hpp"
#include "secs/secs1/serial_port_link.hpp"
#include "secs/secs1/state_machine.hpp"
#include "secs/sml/render.hpp"
#include "secs/sml/runtime.hpp"
#include "secs/utils/protocol_helpers.hpp"

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/this_coro.hpp>

#include <charconv>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace {

using secs::core::byte;
using secs::core::bytes_view;
using secs::core::LogLevel;

using secs::protocol::DataMessage;
using secs::protocol::HandlerResult;
using ProtocolSession = secs::protocol::Session;

using secs::sml::MessageDef;
using secs::sml::Runtime;
using secs::sml::TimerRule;

enum class Role : std::uint8_t {
    host = 0,
    equipment = 1,
};

struct Options final {
    Role role{Role::equipment};
    std::string serial{};
    int baud{9600};
    std::uint16_t device_id{0x0001};
    std::string sml_path{"docs/sml_sample/sample.sml"};

    bool enable_timers{false};
    std::vector<std::string> fire_messages{};

    LogLevel log_level{LogLevel::info};
};

static void print_usage(const char *argv0) {
    std::cout << "用法:\n"
              << "  " << argv0 << " --serial <COMx|/dev/tty*> [options]\n\n"
              << "选项:\n"
              << "  --role <host|equipment>     角色（影响 SECS-I R-bit 方向，默认 equipment）\n"
              << "  --serial <name>             串口名（Windows: COM5/COM10；Linux: /dev/ttyUSB0）\n"
              << "  --baud <n>                  波特率（默认 9600）\n"
              << "  --device-id <u16>           DeviceID（支持 0x 前缀，默认 0x0001）\n"
              << "  --sml <path>                SML 文件路径（默认 docs/sml_sample/sample.sml）\n"
              << "  --enable-timers             启用 SML 的 every N send 规则（串行执行）\n"
              << "  --fire <name_or_SxFy>        启动后发送一次指定消息（可重复）\n"
              << "  --log-level <lvl>           trace|debug|info|warn|error|critical|off（默认 info）\n"
              << "  --help                      显示帮助\n\n"
              << "示例（Windows/com0com）:\n"
              << "  # 本端作为 Equipment，对端（被测 Host）打开 COM6\n"
              << "  " << argv0
              << " --role equipment --serial COM5 --baud 9600 --device-id 0x0001 --sml docs/sml_sample/sample.sml\n\n"
              << "  # 本端作为 Host，对端（被测 Equipment）打开 COM5\n"
              << "  " << argv0
              << " --role host --serial COM6 --baud 9600 --device-id 0x0001 --sml docs/sml_sample/sample.sml --enable-timers\n";
}

static bool parse_u16(std::string_view s, std::uint16_t &out) {
    unsigned v = 0;
    int base = 10;
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s.remove_prefix(2);
    }

    auto *begin = s.data();
    auto *end = s.data() + s.size();
    auto [ptr, ec] = std::from_chars(begin, end, v, base);
    if (ec != std::errc{} || ptr != end || v > 0xFFFFu) {
        return false;
    }
    out = static_cast<std::uint16_t>(v);
    return true;
}

static bool parse_i32(std::string_view s, int &out) {
    int v = 0;
    auto *begin = s.data();
    auto *end = s.data() + s.size();
    auto [ptr, ec] = std::from_chars(begin, end, v, 10);
    if (ec != std::errc{} || ptr != end) {
        return false;
    }
    out = v;
    return true;
}

static bool parse_role(std::string_view s, Role &out) {
    if (s == "host") {
        out = Role::host;
        return true;
    }
    if (s == "equipment" || s == "eq") {
        out = Role::equipment;
        return true;
    }
    return false;
}

static bool parse_log_level(std::string_view s, LogLevel &out) {
    if (s == "trace") {
        out = LogLevel::trace;
        return true;
    }
    if (s == "debug") {
        out = LogLevel::debug;
        return true;
    }
    if (s == "info") {
        out = LogLevel::info;
        return true;
    }
    if (s == "warn") {
        out = LogLevel::warn;
        return true;
    }
    if (s == "error") {
        out = LogLevel::error;
        return true;
    }
    if (s == "critical") {
        out = LogLevel::critical;
        return true;
    }
    if (s == "off") {
        out = LogLevel::off;
        return true;
    }
    return false;
}

static std::optional<std::string> read_file_text(const std::string &path,
                                                 std::string &out_err) {
    std::ifstream f(path, std::ios::in | std::ios::binary);
    if (!f.is_open()) {
        out_err = "open failed";
        return std::nullopt;
    }
    f.seekg(0, std::ios::end);
    const std::streamoff size = f.tellg();
    if (size < 0) {
        out_err = "tellg failed";
        return std::nullopt;
    }
    f.seekg(0, std::ios::beg);

    std::string buf;
    buf.resize(static_cast<std::size_t>(size));
    if (!buf.empty()) {
        f.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        if (!f) {
            out_err = "read failed";
            return std::nullopt;
        }
    }
    return buf;
}

static std::optional<const MessageDef *>
resolve_message(const Runtime &rt, std::string_view name_or_sf) {
    if (const auto *msg = rt.get_message(name_or_sf)) {
        return msg;
    }
    return std::nullopt;
}

static void print_message_header(const char *tag, const DataMessage &msg) {
    std::cout << tag << " S" << static_cast<int>(msg.stream) << "F"
              << static_cast<int>(msg.function) << " W=" << (msg.w_bit ? 1 : 0)
              << " SB=0x" << std::hex << msg.system_bytes << std::dec
              << " body_n=" << msg.body.size() << "\n";
}

static secs::protocol::Handler
make_sml_auto_reply(std::shared_ptr<Runtime> rt) {
    return [rt](const DataMessage &req) -> asio::awaitable<HandlerResult> {
        try {
            print_message_header("[in]", req);

            // W=0：协议层不会回包，这里仅作为“已处理”返回 OK，避免不必要的噪声。
            if (!req.w_bit) {
                co_return HandlerResult{std::error_code{}, {}};
            }
            if (req.function == 0xFFu) {
                co_return HandlerResult{
                    secs::core::make_error_code(secs::core::errc::invalid_argument),
                    {}};
            }

            // 解码入站 body（无 body 时使用空 List；便于匹配仅依赖 SxFy 的规则）。
            secs::ii::Item decoded{secs::ii::List{}};
            {
                auto [dec_ec, decoded_opt] = secs::utils::decode_one_item_if_any(
                    bytes_view{req.body.data(), req.body.size()});
                if (dec_ec) {
                    co_return HandlerResult{dec_ec, {}};
                }
                if (decoded_opt.has_value()) {
                    decoded = std::move(decoded_opt->item);
                }
            }

            auto matched = rt->match_response(req.stream, req.function, decoded);
            if (!matched.has_value()) {
                std::cout << "[auto-reply] no match\n";
                co_return HandlerResult{
                    secs::core::make_error_code(secs::core::errc::invalid_argument),
                    {}};
            }

            const auto *rsp = rt->get_message(*matched);
            if (!rsp) {
                std::cout << "[auto-reply] response not found: " << *matched
                          << "\n";
                co_return HandlerResult{
                    secs::core::make_error_code(secs::core::errc::invalid_argument),
                    {}};
            }

            const auto expected_function =
                static_cast<std::uint8_t>(req.function + 1u);
            if (rsp->stream != req.stream || rsp->function != expected_function ||
                rsp->w_bit) {
                std::cout << "[auto-reply] response SF/W mismatch: matched="
                          << *matched << " expected=S" << static_cast<int>(req.stream)
                          << "F" << static_cast<int>(expected_function)
                          << " W=0 but got S" << static_cast<int>(rsp->stream)
                          << "F" << static_cast<int>(rsp->function)
                          << " W=" << (rsp->w_bit ? 1 : 0) << "\n";
                co_return HandlerResult{
                    secs::core::make_error_code(secs::core::errc::invalid_argument),
                    {}};
            }

            // 当前示例未提供变量注入接口：使用空上下文渲染。
            secs::sml::RenderContext ctx{};
            secs::ii::Item rendered{secs::ii::List{}};
            const auto render_ec =
                secs::sml::render_item(rsp->item, ctx, rendered);
            if (render_ec) {
                std::cout << "[auto-reply] render failed: " << render_ec.message()
                          << "\n";
                co_return HandlerResult{render_ec, {}};
            }

            auto result = secs::utils::make_handler_result(rendered);
            if (result.first) {
                std::cout << "[auto-reply] encode failed: "
                          << result.first.message() << "\n";
                co_return HandlerResult{result.first, {}};
            }

            std::cout << "[auto-reply] matched " << *matched
                      << " -> reply body_n=" << result.second.size() << "\n";
            co_return result;
        } catch (const std::bad_alloc &) {
            co_return HandlerResult{
                secs::core::make_error_code(secs::core::errc::out_of_memory), {}};
        } catch (...) {
            co_return HandlerResult{
                secs::core::make_error_code(secs::core::errc::invalid_argument),
                {}};
        }
    };
}

static asio::awaitable<void>
fire_once(ProtocolSession &proto, std::shared_ptr<Runtime> rt, std::string name_or_sf) {
    const auto msg_opt = resolve_message(*rt, name_or_sf);
    if (!msg_opt.has_value()) {
        std::cout << "[fire] message not found: " << name_or_sf << "\n";
        co_return;
    }
    const auto *msg = *msg_opt;

    if (msg->function == 0 || (msg->function & 0x01U) == 0) {
        std::cout << "[fire] not a primary message: " << name_or_sf
                  << " (S" << static_cast<int>(msg->stream) << "F"
                  << static_cast<int>(msg->function) << ")\n";
        co_return;
    }

    // 当前示例未提供变量注入接口：使用空上下文渲染。
    secs::sml::RenderContext ctx{};
    secs::ii::Item rendered{secs::ii::List{}};
    const auto render_ec = secs::sml::render_item(msg->item, ctx, rendered);
    if (render_ec) {
        std::cout << "[fire] render failed: " << name_or_sf
                  << " ec=" << render_ec.message() << "\n";
        co_return;
    }

    if (msg->w_bit) {
        std::cout << "[fire] request " << name_or_sf << " -> S"
                  << static_cast<int>(msg->stream) << "F"
                  << static_cast<int>(msg->function) << "\n";

        auto [ec, out] = co_await secs::utils::async_request_decoded(
            proto, msg->stream, msg->function, rendered, std::nullopt);
        if (ec && out.reply.function == 0) {
            std::cout << "[fire] request failed: " << ec.message() << "\n";
            co_return;
        }
        if (ec) {
            std::cout << "[fire] response decode failed: " << ec.message()
                      << "\n";
        }
        print_message_header("[fire][rsp]", out.reply);
        co_return;
    }

    std::cout << "[fire] send " << name_or_sf << " -> S"
              << static_cast<int>(msg->stream) << "F"
              << static_cast<int>(msg->function) << "\n";
    const auto ec = co_await secs::utils::async_send_item(
        proto, msg->stream, msg->function, rendered);
    if (ec) {
        std::cout << "[fire] send failed: " << ec.message() << "\n";
    }
}

struct TimerState final {
    TimerRule rule{};
    secs::core::steady_clock::time_point next_fire{};
};

static std::optional<secs::core::steady_clock::time_point>
earliest_deadline(const std::vector<TimerState> &timers) {
    if (timers.empty()) {
        return std::nullopt;
    }
    auto t = timers.front().next_fire;
    for (const auto &s : timers) {
        if (s.next_fire < t) {
            t = s.next_fire;
        }
    }
    return t;
}

static int parse_args(int argc, char **argv, Options &out) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];

        auto need_value = [&](const char *flag) -> std::optional<std::string_view> {
            if (i + 1 >= argc) {
                std::cerr << "missing value for " << flag << "\n";
                return std::nullopt;
            }
            return std::string_view(argv[++i]);
        };

        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 1;
        }

        if (arg == "--role") {
            auto v = need_value("--role");
            if (!v.has_value()) {
                return -1;
            }
            if (!parse_role(*v, out.role)) {
                std::cerr << "invalid --role: " << *v << "\n";
                return -1;
            }
            continue;
        }

        if (arg == "--serial") {
            auto v = need_value("--serial");
            if (!v.has_value()) {
                return -1;
            }
            out.serial = std::string(*v);
            continue;
        }

        if (arg == "--baud") {
            auto v = need_value("--baud");
            if (!v.has_value()) {
                return -1;
            }
            int b = 0;
            if (!parse_i32(*v, b) || b <= 0) {
                std::cerr << "invalid --baud: " << *v << "\n";
                return -1;
            }
            out.baud = b;
            continue;
        }

        if (arg == "--device-id") {
            auto v = need_value("--device-id");
            if (!v.has_value()) {
                return -1;
            }
            std::uint16_t d = 0;
            if (!parse_u16(*v, d)) {
                std::cerr << "invalid --device-id: " << *v << "\n";
                return -1;
            }
            out.device_id = d;
            continue;
        }

        if (arg == "--sml") {
            auto v = need_value("--sml");
            if (!v.has_value()) {
                return -1;
            }
            out.sml_path = std::string(*v);
            continue;
        }

        if (arg == "--enable-timers") {
            out.enable_timers = true;
            continue;
        }

        if (arg == "--fire") {
            auto v = need_value("--fire");
            if (!v.has_value()) {
                return -1;
            }
            out.fire_messages.emplace_back(*v);
            continue;
        }

        if (arg == "--log-level") {
            auto v = need_value("--log-level");
            if (!v.has_value()) {
                return -1;
            }
            if (!parse_log_level(*v, out.log_level)) {
                std::cerr << "invalid --log-level: " << *v << "\n";
                return -1;
            }
            continue;
        }

        std::cerr << "unknown arg: " << arg << "\n";
        return -1;
    }

    if (out.serial.empty()) {
        std::cerr << "missing required --serial\n";
        return -1;
    }

    return 0;
}

asio::awaitable<int> run_peer(const Options &opt) {
    auto ex = co_await asio::this_coro::executor;

    secs::core::set_log_level(opt.log_level);

    std::string file_err;
    const auto text_opt = read_file_text(opt.sml_path, file_err);
    if (!text_opt.has_value()) {
        std::cout << "[sml] read failed: path=" << opt.sml_path
                  << " err=" << file_err << "\n";
        co_return 2;
    }

    auto rt = std::make_shared<Runtime>();
    const auto load_ec = rt->load(*text_opt);
    if (load_ec) {
        std::cout << "[sml] load failed: " << load_ec.message() << "\n";
        co_return 3;
    }

    std::cout << "[secs1] open serial: " << opt.serial << " baud=" << opt.baud
              << " device_id=0x" << std::hex << opt.device_id << std::dec
              << " role=" << (opt.role == Role::host ? "host" : "equipment")
              << "\n";

    auto [open_ec, link] =
        secs::secs1::SerialPortLink::open(ex, opt.serial, opt.baud);
    if (open_ec) {
        std::cout << "[secs1] open failed: " << open_ec.message() << "\n";
        co_return 4;
    }

    // SECS-I 传输层状态机：expected_device_id 用于校验入站消息头。
    secs::secs1::Timeouts secs1_timeouts{};
    secs1_timeouts.t3_reply = 45s;
    secs::secs1::StateMachine sm(link, opt.device_id, secs1_timeouts);

    secs::protocol::SessionOptions proto_opt{};
    proto_opt.t3 = 45s;
    proto_opt.poll_interval = 50ms;
    proto_opt.secs1_reverse_bit = (opt.role == Role::equipment);

    ProtocolSession proto(sm, opt.device_id, proto_opt);
    proto.router().set_default(make_sml_auto_reply(rt));

    for (const auto &name : opt.fire_messages) {
        co_await fire_once(proto, rt, name);
    }

    std::vector<TimerState> timers;
    if (opt.enable_timers) {
        const auto now = secs::core::steady_clock::now();
        for (const auto &rule : rt->timers()) {
            if (rule.interval_seconds <= 0) {
                continue;
            }
            TimerState st{};
            st.rule = rule;
            st.next_fire = now + std::chrono::seconds(rule.interval_seconds);
            timers.push_back(st);
            std::cout << "[timer] every " << rule.interval_seconds << "s send "
                      << rule.message_name << "\n";
        }
    }

    // 主循环：串行驱动“收包处理”和“timer/fire 发送”，避免半双工并发读写。
    for (;;) {
        // 先处理到期 timer（可能一次触发多个）。
        if (!timers.empty()) {
            const auto now = secs::core::steady_clock::now();
            bool did_fire = false;
            for (auto &t : timers) {
                if (t.next_fire > now) {
                    continue;
                }
                did_fire = true;
                co_await fire_once(proto, rt, t.rule.message_name);
                const auto interval = std::chrono::seconds(t.rule.interval_seconds);
                // 防止长时间阻塞后“追赶触发”造成的密集发送：这里按当前时间重新对齐下一次。
                t.next_fire = secs::core::steady_clock::now() + interval;
            }
            if (did_fire) {
                continue;
            }
        }

        std::optional<secs::core::duration> wait = 500ms;
        if (const auto ddl = earliest_deadline(timers); ddl.has_value()) {
            const auto now = secs::core::steady_clock::now();
            if (*ddl <= now) {
                wait = secs::core::duration{};
            } else {
                wait = *ddl - now;
            }
        }

        const auto ec = co_await proto.async_poll_once(wait);
        if (ec == secs::core::make_error_code(secs::core::errc::timeout)) {
            continue;
        }
        if (ec) {
            std::cout << "[secs1] poll failed: " << ec.message() << "\n";
            proto.stop();
            co_return 5;
        }
    }
}

} // namespace

int main(int argc, char **argv) {
    Options opt{};
    const int parse_rc = parse_args(argc, argv, opt);
    if (parse_rc != 0) {
        return parse_rc > 0 ? 0 : 1;
    }

    asio::io_context ioc;
    int rc = 1;

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            rc = co_await run_peer(opt);
            ioc.stop();
        },
        asio::detached);

    ioc.run();
    return rc;
}
