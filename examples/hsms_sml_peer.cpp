/**
 * @file hsms_sml_peer.cpp
 * @brief HSMS（TCP）对端测试工具：主动/被动都加载同一份 SML，并按 SML 规则自动回包
 *
 * 目标场景：
 * - 你有一个 Windows 测试应用（可配置 HSMS 主动/被动），而本库在 WSL 开发；
 * - 希望在 WSL 侧跑一个“对端”程序，通过加载同一份 SML（例如 docs/sml_sample/sample.sml）
 *   来验证双方互通是否正常；
 * - 该示例支持：
 *   - passive：监听并等待对端连接（对端为 active）
 *   - active：主动连接到对端（对端为 passive）
 *   - 自动回包：收到 primary 且 W=1 时，按 SML 条件规则匹配响应模板，并回
 *     secondary（SxF(y+1), W=0）
 *   - 可选：按 SML 的 every N send 规则周期性发送消息（用于联调时“自动出流量”）
 *
 * 用法：
 *   ./hsms_sml_peer --help
 *
 * 常用：
 *   # 1) WSL 侧作为被动端（监听），Windows 测试应用作为主动端连接
 *   ./hsms_sml_peer --mode passive --listen 0.0.0.0 --port 5000 --sml docs/sml_sample/sample.sml --session-id 0x0001
 *
 *   # 2) WSL 侧作为主动端（连接），Windows 测试应用作为被动端监听
 *   ./hsms_sml_peer --mode active --connect <windows_ip> --port 5000 --sml docs/sml_sample/sample.sml --session-id 0x0001
 */

#include "secs/core/log.hpp"
#include "secs/hsms/session.hpp"
#include "secs/ii/codec.hpp"
#include "secs/protocol/session.hpp"
#include "secs/sml/runtime.hpp"

#include <asio/as_tuple.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/address.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/signal_set.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

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

using secs::hsms::Session;
using secs::hsms::SessionOptions;

using secs::protocol::DataMessage;
using secs::protocol::HandlerResult;
using ProtocolSession = secs::protocol::Session;
using ProtocolOptions = secs::protocol::SessionOptions;

using secs::sml::MessageDef;
using secs::sml::Runtime;
using secs::sml::TimerRule;

enum class Mode : std::uint8_t {
    active = 0,
    passive = 1,
};

struct Options final {
    Mode mode{Mode::passive};
    std::string listen_ip{"0.0.0.0"};
    std::string connect_ip{"127.0.0.1"};
    std::uint16_t port{5000};
    std::uint16_t session_id{0x0001};
    std::string sml_path{"docs/sml_sample/sample.sml"};

    bool enable_timers{false};
    std::vector<std::string> fire_messages{};

    LogLevel log_level{LogLevel::info};
};

static void print_usage(const char *argv0) {
    std::cout << "用法:\n"
              << "  " << argv0
              << " --mode <active|passive> [options]\n\n"
              << "选项:\n"
              << "  --mode <active|passive>     运行模式\n"
              << "  --listen <ip>               passive 模式绑定地址（默认 0.0.0.0）\n"
              << "  --connect <ip>              active 模式连接地址（默认 127.0.0.1）\n"
              << "  --port <port>               端口（默认 5000）\n"
              << "  --session-id <u16>          HSMS data 的 SessionID（支持 0x 前缀，默认 0x0001）\n"
              << "  --sml <path>                SML 文件路径（默认 docs/sml_sample/sample.sml）\n"
              << "  --enable-timers             启用 SML 的 every N send 规则\n"
              << "  --fire <name_or_SxFy>        启动后发送一次指定消息（可重复）\n"
              << "  --log-level <lvl>           trace|debug|info|warn|error|critical|off（默认 info）\n"
              << "  --help                      显示帮助\n\n"
              << "示例:\n"
              << "  # WSL 作为被动端（监听），Windows 应用作为主动端连接\n"
              << "  " << argv0
              << " --mode passive --listen 0.0.0.0 --port 5000 --sml docs/sml_sample/sample.sml --session-id 0x0001\n\n"
              << "  # WSL 作为主动端（连接），Windows 应用作为被动端监听\n"
              << "  " << argv0
              << " --mode active --connect <windows_ip> --port 5000 --sml docs/sml_sample/sample.sml --session-id 0x0001\n";
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

static bool parse_u16_dec(std::string_view s, std::uint16_t &out) {
    unsigned v = 0;
    auto *begin = s.data();
    auto *end = s.data() + s.size();
    auto [ptr, ec] = std::from_chars(begin, end, v, 10);
    if (ec != std::errc{} || ptr != end || v > 0xFFFFu) {
        return false;
    }
    out = static_cast<std::uint16_t>(v);
    return true;
}

static bool parse_mode(std::string_view s, Mode &out) {
    if (s == "active") {
        out = Mode::active;
        return true;
    }
    if (s == "passive") {
        out = Mode::passive;
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

static asio::awaitable<void>
fire_once(std::shared_ptr<ProtocolSession> proto,
          std::shared_ptr<Runtime> rt,
          std::string name_or_sf) {
    auto ex = co_await asio::this_coro::executor;
    (void)ex;

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

    std::vector<byte> body;
    const auto enc_ec = secs::ii::encode(msg->item, body);
    if (enc_ec) {
        std::cout << "[fire] encode failed: " << name_or_sf
                  << " ec=" << enc_ec.message() << "\n";
        co_return;
    }

    if (msg->w_bit) {
        std::cout << "[fire] request " << name_or_sf << " -> S"
                  << static_cast<int>(msg->stream) << "F"
                  << static_cast<int>(msg->function) << " (body=" << body.size()
                  << ")\n";
        auto [ec, rsp] = co_await proto->async_request(
            msg->stream,
            msg->function,
            bytes_view{body.data(), body.size()},
            std::nullopt);
        if (ec) {
            std::cout << "[fire] request failed: " << ec.message() << "\n";
            co_return;
        }
        print_message_header("[fire][rsp]", rsp);
        co_return;
    }

    std::cout << "[fire] send " << name_or_sf << " -> S"
              << static_cast<int>(msg->stream) << "F"
              << static_cast<int>(msg->function) << " (body=" << body.size()
              << ")\n";
    const auto ec =
        co_await proto->async_send(msg->stream,
                                  msg->function,
                                  bytes_view{body.data(), body.size()});
    if (ec) {
        std::cout << "[fire] send failed: " << ec.message() << "\n";
    }
}

static asio::awaitable<void>
timer_loop(std::shared_ptr<ProtocolSession> proto,
           std::shared_ptr<Runtime> rt,
           TimerRule rule) {
    auto ex = co_await asio::this_coro::executor;
    asio::steady_timer timer(ex);

    const auto interval = std::chrono::seconds(rule.interval_seconds);
    std::cout << "[timer] every " << rule.interval_seconds << "s send "
              << rule.message_name << "\n";

    while (true) {
        timer.expires_after(interval);
        auto [ec] =
            co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
        if (ec == asio::error::operation_aborted) {
            co_return;
        }
        if (ec) {
            std::cout << "[timer] wait error: " << ec.message() << "\n";
            co_return;
        }
        co_await fire_once(proto, rt, rule.message_name);
    }
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
            if (!req.body.empty()) {
                std::size_t consumed = 0;
                const auto dec_ec = secs::ii::decode_one(
                    bytes_view{req.body.data(), req.body.size()}, decoded, consumed);
                if (dec_ec) {
                    co_return HandlerResult{dec_ec, {}};
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

            std::vector<byte> body;
            const auto enc_ec = secs::ii::encode(rsp->item, body);
            if (enc_ec) {
                std::cout << "[auto-reply] encode failed: " << enc_ec.message()
                          << "\n";
                co_return HandlerResult{enc_ec, {}};
            }

            std::cout << "[auto-reply] matched " << *matched
                      << " -> reply body_n=" << body.size() << "\n";
            co_return HandlerResult{std::error_code{}, std::move(body)};
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
run_protocol_session(std::shared_ptr<Session> hsms,
                     const Options &opt,
                     std::shared_ptr<Runtime> rt) {
    auto ex = co_await asio::this_coro::executor;

    ProtocolOptions proto_opt{};
    proto_opt.t3 = 45s;
    proto_opt.poll_interval = 10ms;

    auto proto =
        std::make_shared<ProtocolSession>(*hsms, opt.session_id, proto_opt);
    proto->router().set_default(make_sml_auto_reply(rt));

    asio::co_spawn(ex,
                   [proto]() -> asio::awaitable<void> {
                       co_await proto->async_run();
                   },
                   asio::detached);

    if (opt.enable_timers) {
        for (const auto &rule : rt->timers()) {
            asio::co_spawn(ex,
                           timer_loop(proto, rt, rule),
                           asio::detached);
        }
    }

    for (const auto &name : opt.fire_messages) {
        asio::co_spawn(ex, fire_once(proto, rt, name), asio::detached);
    }

    const auto wait_ec = co_await hsms->async_wait_reader_stopped(std::nullopt);
    if (wait_ec) {
        std::cout << "[hsms] reader stopped: " << wait_ec.message() << "\n";
    } else {
        std::cout << "[hsms] reader stopped\n";
    }
}

static SessionOptions make_hsms_options(const Options &opt) {
    SessionOptions out{};
    out.session_id = opt.session_id;
    out.t3 = 45s;
    out.t5 = 2s;
    out.t6 = 5s;
    out.t7 = 10s;
    out.t8 = 5s;
    out.linktest_interval = 0s;
    out.auto_reconnect = false;
    out.passive_accept_select = true;
    return out;
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

        if (arg == "--mode") {
            auto v = need_value("--mode");
            if (!v.has_value()) {
                return -1;
            }
            if (!parse_mode(*v, out.mode)) {
                std::cerr << "invalid --mode: " << *v << "\n";
                return -1;
            }
            continue;
        }

        if (arg == "--listen") {
            auto v = need_value("--listen");
            if (!v.has_value()) {
                return -1;
            }
            out.listen_ip = std::string(*v);
            continue;
        }

        if (arg == "--connect") {
            auto v = need_value("--connect");
            if (!v.has_value()) {
                return -1;
            }
            out.connect_ip = std::string(*v);
            continue;
        }

        if (arg == "--port") {
            auto v = need_value("--port");
            if (!v.has_value()) {
                return -1;
            }
            std::uint16_t p = 0;
            if (!parse_u16_dec(*v, p) || p == 0) {
                std::cerr << "invalid --port: " << *v << "\n";
                return -1;
            }
            out.port = p;
            continue;
        }

        if (arg == "--session-id") {
            auto v = need_value("--session-id");
            if (!v.has_value()) {
                return -1;
            }
            std::uint16_t sid = 0;
            if (!parse_u16(*v, sid)) {
                std::cerr << "invalid --session-id: " << *v << "\n";
                return -1;
            }
            out.session_id = sid;
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
            LogLevel lvl{};
            if (!parse_log_level(*v, lvl)) {
                std::cerr << "invalid --log-level: " << *v << "\n";
                return -1;
            }
            out.log_level = lvl;
            continue;
        }

        std::cerr << "unknown arg: " << arg << "\n";
        return -1;
    }

    return 0;
}

} // namespace

int main(int argc, char **argv) {
    std::cout << "=== HSMS SML peer ===\n\n";

    Options opt{};
    const int parse_rc = parse_args(argc, argv, opt);
    if (parse_rc != 0) {
        if (parse_rc > 0) {
            return 0;
        }
        print_usage(argv[0]);
        return 2;
    }

    secs::core::set_log_level(opt.log_level);

    std::string err;
    const auto content_opt = read_file_text(opt.sml_path, err);
    if (!content_opt.has_value()) {
        std::cerr << "[sml] read failed: " << opt.sml_path << " (" << err
                  << ")\n";
        return 1;
    }

    // 先用 parse_sml 获取更详细的错误信息（行/列/上下文），再加载到 Runtime。
    const auto parsed = secs::sml::parse_sml(*content_opt);
    if (parsed.ec) {
        std::cerr << "[sml] parse failed: ec=" << parsed.ec.message()
                  << " line=" << parsed.error_line
                  << " col=" << parsed.error_column << " msg="
                  << parsed.error_message << "\n";
        return 1;
    }

    auto rt = std::make_shared<Runtime>();
    rt->load(parsed.document);
    if (!rt->loaded()) {
        std::cerr << "[sml] load failed: build index error\n";
        return 1;
    }

    std::cout << "[sml] loaded: " << opt.sml_path << "\n";
    std::cout << "[sml] messages=" << rt->messages().size()
              << " conditions=" << rt->conditions().size()
              << " timers=" << rt->timers().size() << "\n";

    try {
        asio::io_context ioc;

        asio::signal_set signals(ioc, SIGINT, SIGTERM);

        const auto hsms_opt = make_hsms_options(opt);

        if (opt.mode == Mode::active) {
            std::error_code addr_ec;
            const auto addr = asio::ip::make_address(opt.connect_ip, addr_ec);
            if (addr_ec) {
                std::cerr << "invalid --connect ip: " << opt.connect_ip << "\n";
                return 2;
            }
            asio::ip::tcp::endpoint endpoint(addr, opt.port);

            auto hsms = std::make_shared<Session>(ioc.get_executor(), hsms_opt);

            // Ctrl+C：停止会话并退出。
            signals.async_wait([&](const std::error_code &, int) {
                std::cout << "\n[main] stop requested\n";
                hsms->stop();
                ioc.stop();
            });

            asio::co_spawn(
                ioc,
                [hsms, endpoint, opt, rt]() -> asio::awaitable<void> {
                    std::cout << "[active] connect to " << endpoint.address()
                              << ":" << endpoint.port() << "\n";
                    const auto ec = co_await hsms->async_open_active(endpoint);
                    if (ec) {
                        std::cout << "[active] open failed: " << ec.message()
                                  << "\n";
                        co_return;
                    }
                    std::cout << "[active] selected\n";
                    co_await run_protocol_session(hsms, opt, rt);
                },
                asio::detached);

            ioc.run();
            hsms->stop();
            return 0;
        }

        // passive 模式：主线程持有 acceptor，便于 signal handler 关闭。
        std::error_code addr_ec;
        const auto addr = asio::ip::make_address(opt.listen_ip, addr_ec);
        if (addr_ec) {
            std::cerr << "invalid --listen ip: " << opt.listen_ip << "\n";
            return 2;
        }

        asio::ip::tcp::endpoint listen_ep(addr, opt.port);
        asio::ip::tcp::acceptor acceptor(ioc, listen_ep);
        std::cout << "[passive] listen on " << listen_ep.address() << ":"
                  << listen_ep.port() << "\n";

        // signal 时关闭 acceptor，打断 accept。
        signals.async_wait([&](const std::error_code &, int) {
            std::cout << "\n[main] stop requested\n";
            acceptor.close();
            ioc.stop();
        });

        asio::co_spawn(
            ioc,
            [&acceptor, hsms_opt, opt, rt]() -> asio::awaitable<void> {
                while (true) {
                    auto [ec, socket] = co_await acceptor.async_accept(
                        asio::as_tuple(asio::use_awaitable));
                    if (ec) {
                        if (ec == asio::error::operation_aborted) {
                            co_return;
                        }
                        std::cout << "[passive] accept error: " << ec.message()
                                  << "\n";
                        continue;
                    }

                    std::cout << "[passive] accepted from "
                              << socket.remote_endpoint().address() << ":"
                              << socket.remote_endpoint().port() << "\n";

                    auto hsms =
                        std::make_shared<Session>(acceptor.get_executor(), hsms_opt);
                    const auto open_ec =
                        co_await hsms->async_open_passive(std::move(socket));
                    if (open_ec) {
                        std::cout << "[passive] SELECT failed: "
                                  << open_ec.message() << "\n";
                        continue;
                    }
                    std::cout << "[passive] selected\n";
                    co_await run_protocol_session(hsms, opt, rt);
                }
            },
            asio::detached);

        ioc.run();
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "fatal: " << e.what() << "\n";
        return 1;
    }
}
