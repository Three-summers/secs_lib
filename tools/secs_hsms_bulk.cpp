/**
 * @file secs_hsms_bulk.cpp
 * @brief HSMS + SECS-II Binary 大负载传输测试工具
 *
 * 目标：
 * - 测试 HSMS data message 承载 SECS-II Binary（B）的大体量传输；
 * - 支持 loopback（纯内存，不依赖 socket）与 TCP server/client；
 * - 支持 ACK（小响应）与 ECHO（回显大 body）两种模式；
 * - 输出吞吐（MB/s）与基本正确性校验结果。
 *
 * 用法示例：
 *   # 纯内存回环（推荐：不依赖网络权限）
 *   ./secs-hsms-bulk --role loopback --mode ack  --binary-bytes 1048576  --messages 1000
 *   ./secs-hsms-bulk --role loopback --mode echo --binary-bytes 8388608  --messages 50
 *
 *   # TCP：先起 server，再起 client
 *   ./secs-hsms-bulk --role server --listen 0.0.0.0 --port 5000 --messages 100
 *   ./secs-hsms-bulk --role client --connect 127.0.0.1 --port 5000 --binary-bytes 1048576 --messages 100
 *
 * 说明：
 * - 单条 HSMS 帧的 body 上限由库内 `secs::hsms::kMaxPayloadSize` 限制（默认 16MB），
 *   且需要扣除 HSMS header（10B）与 SECS-II Item header（2~4B）。
 * - 若想测试“更大总量”，请通过增大 `--messages` 累计传输字节数。
 */

#include "secs/core/common.hpp"
#include "secs/core/error.hpp"
#include "secs/core/event.hpp"
#include "secs/hsms/connection.hpp"
#include "secs/hsms/message.hpp"
#include "secs/hsms/session.hpp"
#include "secs/ii/item.hpp"
#include "secs/ii/types.hpp"
#include "secs/utils/ii_helpers.hpp"

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

#include <algorithm>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <iomanip>
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

using secs::core::byte;
using secs::core::bytes_view;
using secs::core::errc;
using secs::core::make_error_code;

using secs::hsms::Connection;
using secs::hsms::Message;
using secs::hsms::Session;
using secs::hsms::SessionOptions;
using secs::hsms::Stream;

enum class Role : std::uint8_t {
    server = 0,
    client = 1,
    loopback = 2,
};

enum class Mode : std::uint8_t {
    ack = 0,  // 小响应：返回 SECS-II binary_len 与 body_bytes
    echo = 1, // 回显响应：返回原样 body（大）
};

struct Options final {
    Role role{Role::loopback};
    Mode mode{Mode::ack};

    std::string listen_ip{"0.0.0.0"};
    std::string connect_ip{"127.0.0.1"};
    std::uint16_t port{5000};
    std::uint16_t session_id{0x0001};

    std::uint8_t stream{99};
    std::uint8_t function{1}; // 主消息（奇数）

    std::size_t binary_bytes{1u * 1024u * 1024u}; // 默认 1MiB
    std::uint64_t messages{100};

    std::uint32_t timeout_ms{45'000};
};

struct Counters final {
    std::uint64_t rx_messages{0};
    std::uint64_t tx_messages{0};
    std::uint64_t rx_bytes{0};
    std::uint64_t tx_bytes{0};
};

static void print_usage(const char *argv0) {
    std::cout << "用法:\n"
              << "  " << argv0
              << " --role <server|client|loopback> [options]\n\n"
              << "说明:\n"
              << "  - client 发送 SECS-II Binary item（B）作为 HSMS data message body\n"
              << "  - server 收到主消息后按 mode 回包（ACK 小响应 / ECHO 回显大 body）\n\n"
              << "选项:\n"
              << "  --role <server|client|loopback>\n"
              << "  --mode <ack|echo>        响应模式（默认 ack）\n"
              << "  --listen <ip>            server 监听地址（默认 0.0.0.0）\n"
              << "  --connect <ip>           client 连接地址（默认 127.0.0.1）\n"
              << "  --port <u16>             端口（默认 5000）\n"
              << "  --session-id <u16>       HSMS data SessionID（支持 0x 前缀，默认 0x0001）\n"
              << "  --stream <u8>            Stream（默认 99）\n"
              << "  --function <u8>          Function（主消息，必须为奇数；默认 1）\n"
              << "  --binary-bytes <n>       Binary payload 字节数（默认 1048576=1MiB）\n"
              << "  --messages <u64>         client 发送次数 / server 处理次数（默认 100）\n"
              << "  --timeout-ms <u32>       client 等待响应超时（默认 45000ms）\n"
              << "  -h, --help               显示帮助\n\n"
              << "示例:\n"
              << "  " << argv0
              << " --role loopback --mode ack  --binary-bytes 1048576 --messages 1000\n"
              << "  " << argv0
              << " --role loopback --mode echo --binary-bytes 8388608 --messages 50\n";
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

static bool parse_u8(std::string_view s, std::uint8_t &out) {
    std::uint16_t v = 0;
    if (!parse_u16(s, v) || v > 0xFFu) {
        return false;
    }
    out = static_cast<std::uint8_t>(v);
    return true;
}

static bool parse_u32(std::string_view s, std::uint32_t &out) {
    std::uint64_t v = 0;
    auto *begin = s.data();
    auto *end = s.data() + s.size();
    auto [ptr, ec] = std::from_chars(begin, end, v, 10);
    if (ec != std::errc{} || ptr != end || v > 0xFFFFFFFFull) {
        return false;
    }
    out = static_cast<std::uint32_t>(v);
    return true;
}

static bool parse_u64(std::string_view s, std::uint64_t &out) {
    std::uint64_t v = 0;
    auto *begin = s.data();
    auto *end = s.data() + s.size();
    auto [ptr, ec] = std::from_chars(begin, end, v, 10);
    if (ec != std::errc{} || ptr != end) {
        return false;
    }
    out = v;
    return true;
}

static bool parse_size(std::string_view s, std::size_t &out) {
    std::uint64_t v = 0;
    if (!parse_u64(s, v) || v > static_cast<std::uint64_t>(
                                 std::numeric_limits<std::size_t>::max())) {
        return false;
    }
    out = static_cast<std::size_t>(v);
    return true;
}

static bool parse_role(std::string_view s, Role &out) {
    if (s == "server") {
        out = Role::server;
        return true;
    }
    if (s == "client") {
        out = Role::client;
        return true;
    }
    if (s == "loopback") {
        out = Role::loopback;
        return true;
    }
    return false;
}

static bool parse_mode(std::string_view s, Mode &out) {
    if (s == "ack") {
        out = Mode::ack;
        return true;
    }
    if (s == "echo") {
        out = Mode::echo;
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
        if (a == "--role") {
            const char *v = need_value("--role");
            if (!v || !parse_role(v, opt.role)) {
                std::cerr << "非法 role: " << (v ? v : "") << "\n";
                return std::nullopt;
            }
            continue;
        }
        if (a == "--mode") {
            const char *v = need_value("--mode");
            if (!v || !parse_mode(v, opt.mode)) {
                std::cerr << "非法 mode: " << (v ? v : "") << "\n";
                return std::nullopt;
            }
            continue;
        }
        if (a == "--listen") {
            const char *v = need_value("--listen");
            if (!v || std::string_view{v}.empty()) {
                std::cerr << "非法 listen: " << (v ? v : "") << "\n";
                return std::nullopt;
            }
            opt.listen_ip = v;
            continue;
        }
        if (a == "--connect") {
            const char *v = need_value("--connect");
            if (!v || std::string_view{v}.empty()) {
                std::cerr << "非法 connect: " << (v ? v : "") << "\n";
                return std::nullopt;
            }
            opt.connect_ip = v;
            continue;
        }
        if (a == "--port") {
            std::uint16_t v = 0;
            const char *s = need_value("--port");
            if (!s || !parse_u16(s, v) || v == 0) {
                std::cerr << "非法 port: " << (s ? s : "") << "\n";
                return std::nullopt;
            }
            opt.port = v;
            continue;
        }
        if (a == "--session-id") {
            std::uint16_t v = 0;
            const char *s = need_value("--session-id");
            if (!s || !parse_u16(s, v)) {
                std::cerr << "非法 session-id: " << (s ? s : "") << "\n";
                return std::nullopt;
            }
            opt.session_id = v;
            continue;
        }
        if (a == "--stream") {
            std::uint8_t v = 0;
            const char *s = need_value("--stream");
            if (!s || !parse_u8(s, v) || v > 127) {
                std::cerr << "非法 stream（0..127）: " << (s ? s : "") << "\n";
                return std::nullopt;
            }
            opt.stream = v;
            continue;
        }
        if (a == "--function") {
            std::uint8_t v = 0;
            const char *s = need_value("--function");
            if (!s || !parse_u8(s, v) || v == 0) {
                std::cerr << "非法 function: " << (s ? s : "") << "\n";
                return std::nullopt;
            }
            opt.function = v;
            continue;
        }
        if (a == "--binary-bytes") {
            std::size_t v = 0;
            const char *s = need_value("--binary-bytes");
            if (!s || !parse_size(s, v)) {
                std::cerr << "非法 binary-bytes: " << (s ? s : "") << "\n";
                return std::nullopt;
            }
            opt.binary_bytes = v;
            continue;
        }
        if (a == "--messages") {
            std::uint64_t v = 0;
            const char *s = need_value("--messages");
            if (!s || !parse_u64(s, v) || v == 0) {
                std::cerr << "非法 messages: " << (s ? s : "") << "\n";
                return std::nullopt;
            }
            opt.messages = v;
            continue;
        }
        if (a == "--timeout-ms") {
            std::uint32_t v = 0;
            const char *s = need_value("--timeout-ms");
            if (!s || !parse_u32(s, v) || v == 0) {
                std::cerr << "非法 timeout-ms: " << (s ? s : "") << "\n";
                return std::nullopt;
            }
            opt.timeout_ms = v;
            continue;
        }

        std::cerr << "未知参数: " << a << "\n";
        return std::nullopt;
    }

    // 基本一致性校验（尽量早失败）
    if (opt.function % 2 == 0) {
        std::cerr << "非法 function：主消息必须为奇数（当前 " << int(opt.function)
                  << "）\n";
        return std::nullopt;
    }
    if (opt.function == 255) {
        std::cerr << "非法 function：255 无法生成从消息（function+1 溢出）\n";
        return std::nullopt;
    }
    if (opt.binary_bytes > secs::ii::kMaxLength) {
        std::cerr << "binary-bytes 超过 SECS-II 3 字节长度上限（"
                  << secs::ii::kMaxLength << "）\n";
        return std::nullopt;
    }

    return opt;
}

static std::vector<byte> make_payload(std::size_t n) {
    std::vector<byte> out;
    out.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        out[i] = static_cast<byte>(i & 0xFFu);
    }
    return out;
}

struct Secs2BinaryHeader final {
    std::uint32_t binary_len{0};
    std::size_t header_bytes{0}; // format + len_bytes
};

static std::optional<Secs2BinaryHeader>
parse_secs2_binary_header(bytes_view body) {
    if (body.size() < 2) {
        return std::nullopt;
    }

    const std::uint8_t fmt = body[0];
    const std::uint8_t len_bytes_tag = static_cast<std::uint8_t>(fmt & 0x03U);
    if (len_bytes_tag == 0 || len_bytes_tag > 3) {
        return std::nullopt;
    }

    const std::uint8_t code = static_cast<std::uint8_t>(fmt >> 2U);
    if (code != static_cast<std::uint8_t>(secs::ii::format_code::binary)) {
        return std::nullopt;
    }

    const std::size_t len_bytes = len_bytes_tag;
    if (body.size() < 1 + len_bytes) {
        return std::nullopt;
    }

    std::uint32_t n = 0;
    for (std::size_t i = 0; i < len_bytes; ++i) {
        n = static_cast<std::uint32_t>((n << 8U) |
                                       static_cast<std::uint32_t>(body[1 + i]));
    }

    const std::size_t header_bytes = 1 + len_bytes;
    if (body.size() != header_bytes + static_cast<std::size_t>(n)) {
        return std::nullopt;
    }

    return Secs2BinaryHeader{.binary_len = n, .header_bytes = header_bytes};
}

static double to_mib(std::uint64_t bytes) {
    return static_cast<double>(bytes) / (1024.0 * 1024.0);
}

static void print_throughput(std::string_view label,
                             std::uint64_t bytes,
                             std::chrono::steady_clock::duration elapsed) {
    const double seconds =
        std::chrono::duration_cast<std::chrono::duration<double>>(elapsed)
            .count();
    const double mib = to_mib(bytes);
    const double mibps = seconds > 0.0 ? (mib / seconds) : 0.0;

    std::cout << label << ": bytes=" << bytes << " (" << std::fixed
              << std::setprecision(3) << mib << " MiB)"
              << " elapsed=" << std::setprecision(3) << seconds << "s"
              << " throughput=" << std::setprecision(3) << mibps << " MiB/s\n";
}

static std::uint8_t header_byte2(std::uint8_t stream, bool w_bit) {
    return static_cast<std::uint8_t>((stream & 0x7FU) | (w_bit ? 0x80U : 0U));
}

static Message make_data_message_owned(std::uint16_t session_id,
                                       std::uint8_t stream,
                                       std::uint8_t function,
                                       bool w_bit,
                                       std::uint32_t system_bytes,
                                       std::vector<byte> body) {
    Message m;
    m.header.session_id = session_id;
    m.header.header_byte2 = header_byte2(stream, w_bit);
    m.header.header_byte3 = function;
    m.header.p_type = secs::hsms::kPTypeSecs2;
    m.header.s_type = secs::hsms::SType::data;
    m.header.system_bytes = system_bytes;
    m.body = std::move(body);
    return m;
}

static SessionOptions make_hsms_options(std::uint16_t session_id) {
    SessionOptions hsms_opt{};
    hsms_opt.session_id = session_id;
    hsms_opt.t3 = 45s;
    hsms_opt.t5 = 200ms;
    hsms_opt.t6 = 5s;
    hsms_opt.t7 = 10s;
    hsms_opt.t8 = 5s;
    hsms_opt.linktest_interval = 0s;
    hsms_opt.auto_reconnect = false;
    hsms_opt.passive_accept_select = true;
    return hsms_opt;
}

static std::optional<std::vector<byte>> build_request_body(const Options &opt) {
    // HSMS 单帧 body 上限：kMaxPayloadSize - HSMS header 10B
    const std::size_t max_body_bytes =
        static_cast<std::size_t>(secs::hsms::kMaxPayloadSize) -
        static_cast<std::size_t>(secs::hsms::kHeaderSize);

    // 预估 SECS-II Binary 编码开销：format(1B) + length(1~3B)
    std::size_t len_bytes = 3;
    if (opt.binary_bytes <= 0xFFu) {
        len_bytes = 1;
    } else if (opt.binary_bytes <= 0xFFFFu) {
        len_bytes = 2;
    }
    const std::size_t secs2_header_bytes = 1 + len_bytes;
    if (secs2_header_bytes > max_body_bytes ||
        opt.binary_bytes > (max_body_bytes - secs2_header_bytes)) {
        const std::size_t max_binary_assuming_len3 =
            (max_body_bytes >= 4) ? (max_body_bytes - 4) : 0;
        std::cerr << "[client] binary-bytes 超过 HSMS 单帧上限：\n"
                  << "  - hsms_max_body_bytes=" << max_body_bytes << "\n"
                  << "  - secs2_binary_header_bytes=" << secs2_header_bytes
                  << "\n"
                  << "  - 建议 binary-bytes <= " << max_binary_assuming_len3
                  << "（len_bytes=3 时）\n";
        return std::nullopt;
    }

    // 复用库内 SECS-II codec：构造 Item::binary 并编码为 body bytes
    auto payload = make_payload(opt.binary_bytes);
    const secs::ii::Item item = secs::ii::Item::binary(std::move(payload));
    auto [enc_ec, body] = secs::utils::encode_item(item);
    if (enc_ec) {
        std::cerr << "[client] SECS-II 编码失败: " << enc_ec.message() << "\n";
        return std::nullopt;
    }
    if (body.size() > max_body_bytes) {
        std::cerr << "[client] body 超过 HSMS 单帧上限: body_bytes=" << body.size()
                  << " max=" << max_body_bytes << "\n";
        return std::nullopt;
    }

    // 自检：确认 body 为 Binary 且长度一致（不复制 payload）
    const auto h = parse_secs2_binary_header(bytes_view{body.data(), body.size()});
    if (!h.has_value()) {
        std::cerr << "[client] 生成的 body 不是合法 SECS-II Binary\n";
        return std::nullopt;
    }
    if (h->binary_len != opt.binary_bytes) {
        std::cerr << "[client] body 的 Binary length 与期望不一致: got="
                  << h->binary_len << " expect=" << opt.binary_bytes << "\n";
        return std::nullopt;
    }
    return body;
}

asio::awaitable<int> run_server_loop(Session &sess,
                                     const Options &opt,
                                     Counters &counters) {
    const auto max_messages = opt.messages;
    const auto mode = opt.mode;

    while (true) {
        auto [rx_ec, msg] = co_await sess.async_receive_data();
        if (rx_ec) {
            if (rx_ec == make_error_code(errc::cancelled)) {
                co_return 0;
            }
            std::cout << "[server] recv failed: " << rx_ec.message() << "\n";
            co_return 1;
        }

        ++counters.rx_messages;
        counters.rx_bytes += msg.body.size();

        if (msg.is_data() && msg.w_bit() && (msg.function() & 0x01U) != 0) {
            std::vector<byte> rsp_body;
            if (mode == Mode::echo) {
                rsp_body = std::move(msg.body);
            } else {
                const auto parsed = parse_secs2_binary_header(
                    bytes_view{msg.body.data(), msg.body.size()});
                if (!parsed.has_value()) {
                    std::cout << "[server] 非法 SECS-II Binary body\n";
                    co_return 2;
                }

                const secs::ii::Item ack =
                    secs::ii::Item::u4({parsed->binary_len,
                                        static_cast<std::uint32_t>(msg.body.size())});
                auto [enc_ec, body] = secs::utils::encode_item(ack);
                if (enc_ec) {
                    std::cout << "[server] ACK 编码失败: " << enc_ec.message()
                              << "\n";
                    co_return 3;
                }
                rsp_body = std::move(body);
            }

            const auto stream = msg.stream();
            const auto req_func = msg.function();
            const auto rsp_func = static_cast<std::uint8_t>(req_func + 1);
            const auto sb = msg.header.system_bytes;

            auto rsp = make_data_message_owned(opt.session_id,
                                               stream,
                                               rsp_func,
                                               /*w_bit=*/false,
                                               sb,
                                               std::move(rsp_body));

            const auto tx_ec = co_await sess.async_send(rsp);
            if (tx_ec) {
                std::cout << "[server] send failed: " << tx_ec.message()
                          << "\n";
                co_return 4;
            }

            ++counters.tx_messages;
            counters.tx_bytes += rsp.body.size();
        }

        if (max_messages > 0 && counters.rx_messages >= max_messages) {
            co_return 0;
        }
    }
}

asio::awaitable<int> run_client_loop(Session &sess, const Options &opt) {
    const auto body_opt = build_request_body(opt);
    if (!body_opt.has_value()) {
        co_return 2;
    }
    const auto &req_body = *body_opt;

    std::cout << "[client] stream=" << static_cast<int>(opt.stream)
              << " function=" << static_cast<int>(opt.function)
              << " mode=" << (opt.mode == Mode::echo ? "echo" : "ack")
              << " body_bytes=" << req_body.size()
              << " binary_bytes=" << opt.binary_bytes
              << " messages=" << opt.messages << "\n";

    Counters counters{};
    const auto begin = std::chrono::steady_clock::now();

    for (std::uint64_t i = 0; i < opt.messages; ++i) {
        auto [ec, rsp] = co_await sess.async_request_data(
            opt.stream,
            opt.function,
            bytes_view{req_body.data(), req_body.size()},
            std::chrono::milliseconds(opt.timeout_ms));
        if (ec) {
            std::cout << "[client] request failed at i=" << i
                      << ": " << ec.message() << "\n";
            co_return 3;
        }

        ++counters.tx_messages;
        counters.tx_bytes += req_body.size();
        ++counters.rx_messages;
        counters.rx_bytes += rsp.body.size();

        const auto expect_rsp_func =
            static_cast<std::uint8_t>(opt.function + 1);
        if (!rsp.is_data() || rsp.w_bit() || rsp.stream() != opt.stream ||
            rsp.function() != expect_rsp_func) {
            std::cout << "[client] 响应头不符合预期: stream="
                      << static_cast<int>(rsp.stream())
                      << " function=" << static_cast<int>(rsp.function())
                      << " w=" << (rsp.w_bit() ? 1 : 0) << "\n";
            co_return 4;
        }

        if (opt.mode == Mode::echo) {
            if (rsp.body != req_body) {
                std::cout << "[client] echo 校验失败: rsp.body != req.body\n";
                co_return 5;
            }
        } else {
            auto [dec_ec, decoded] = secs::utils::decode_one_item(
                bytes_view{rsp.body.data(), rsp.body.size()});
            if (dec_ec || !decoded.fully_consumed) {
                std::cout << "[client] ACK 解码失败: "
                          << (dec_ec ? dec_ec.message() : "not fully consumed")
                          << "\n";
                co_return 6;
            }
            const auto *u4 = decoded.item.get_if<secs::ii::U4>();
            if (!u4 || u4->values.size() != 2) {
                std::cout << "[client] ACK 类型不符合预期（期望 U4[2]）\n";
                co_return 7;
            }

            const std::uint32_t ack_binary_len = u4->values[0];
            const std::uint32_t ack_body_bytes = u4->values[1];
            if (ack_binary_len != opt.binary_bytes ||
                ack_body_bytes != req_body.size()) {
                std::cout << "[client] ACK 值不匹配: got{binary_len="
                          << ack_binary_len << ", body_bytes=" << ack_body_bytes
                          << "} expect{binary_len=" << opt.binary_bytes
                          << ", body_bytes=" << req_body.size() << "}\n";
                co_return 8;
            }
        }
    }

    const auto end = std::chrono::steady_clock::now();
    const auto elapsed = end - begin;

    std::cout << "[client] done: tx_messages=" << counters.tx_messages
              << " rx_messages=" << counters.rx_messages << "\n";
    print_throughput("[client] tx", counters.tx_bytes, elapsed);
    print_throughput("[client] rx", counters.rx_bytes, elapsed);
    print_throughput("[client] total", counters.tx_bytes + counters.rx_bytes, elapsed);

    co_return 0;
}

struct MemoryChannel final {
    std::deque<byte> buf{};
    secs::core::Event data_event{};
    bool closed{false};
};

class MemoryStream final : public Stream {
public:
    MemoryStream(asio::any_io_executor ex,
                 std::shared_ptr<MemoryChannel> inbox,
                 std::shared_ptr<MemoryChannel> outbox)
        : ex_(ex), inbox_(std::move(inbox)), outbox_(std::move(outbox)) {}

    [[nodiscard]] asio::any_io_executor executor() const noexcept override {
        return ex_;
    }
    [[nodiscard]] bool is_open() const noexcept override { return open_; }

    void cancel() noexcept override {
        if (inbox_) {
            inbox_->data_event.cancel();
        }
    }

    void close() noexcept override {
        if (!open_) {
            return;
        }
        open_ = false;
        if (outbox_) {
            outbox_->closed = true;
            outbox_->data_event.set();
        }
        if (inbox_) {
            inbox_->data_event.cancel();
        }
    }

    asio::awaitable<std::pair<std::error_code, std::size_t>>
    async_read_some(secs::core::mutable_bytes_view dst) override {
        if (!inbox_) {
            co_return std::pair{make_error_code(errc::invalid_argument),
                                std::size_t{0}};
        }

        while (inbox_->buf.empty()) {
            if (inbox_->closed) {
                co_return std::pair{std::make_error_code(std::errc::broken_pipe),
                                    std::size_t{0}};
            }
            auto ec = co_await inbox_->data_event.async_wait(std::nullopt);
            if (ec) {
                co_return std::pair{ec, std::size_t{0}};
            }
        }

        const std::size_t n = std::min(dst.size(), inbox_->buf.size());
        for (std::size_t i = 0; i < n; ++i) {
            dst[i] = inbox_->buf.front();
            inbox_->buf.pop_front();
        }
        if (inbox_->buf.empty()) {
            inbox_->data_event.reset();
        }
        co_return std::pair{std::error_code{}, n};
    }

    asio::awaitable<std::error_code> async_write_all(bytes_view src) override {
        if (!open_) {
            co_return make_error_code(errc::cancelled);
        }
        if (!outbox_) {
            co_return make_error_code(errc::invalid_argument);
        }
        outbox_->buf.insert(outbox_->buf.end(), src.begin(), src.end());
        outbox_->data_event.set();
        co_return std::error_code{};
    }

    asio::awaitable<std::error_code>
    async_connect(const asio::ip::tcp::endpoint &) override {
        co_return make_error_code(errc::invalid_argument);
    }

private:
    asio::any_io_executor ex_{};
    std::shared_ptr<MemoryChannel> inbox_{};
    std::shared_ptr<MemoryChannel> outbox_{};
    bool open_{true};
};

struct MemoryDuplex final {
    std::unique_ptr<Stream> client_stream;
    std::unique_ptr<Stream> server_stream;
};

static MemoryDuplex make_memory_duplex(asio::any_io_executor ex) {
    auto c2s = std::make_shared<MemoryChannel>();
    auto s2c = std::make_shared<MemoryChannel>();

    MemoryDuplex duplex;
    duplex.client_stream = std::make_unique<MemoryStream>(ex, s2c, c2s);
    duplex.server_stream = std::make_unique<MemoryStream>(ex, c2s, s2c);
    return duplex;
}

asio::awaitable<int> run_loopback(const Options &opt) {
    auto ex = co_await asio::this_coro::executor;

    auto duplex = make_memory_duplex(ex);
    auto hsms_opt = make_hsms_options(opt.session_id);

    Session server(ex, hsms_opt);
    Session client(ex, hsms_opt);

    secs::core::Event server_open_done{};
    std::error_code open_ec_server{};
    asio::co_spawn(
        ex,
        [&]() -> asio::awaitable<void> {
            Connection c(std::move(duplex.server_stream));
            open_ec_server = co_await server.async_open_passive(std::move(c));
            server_open_done.set();
        },
        asio::detached);

    Connection c(std::move(duplex.client_stream));
    const auto open_ec_client = co_await client.async_open_active(std::move(c));
    const auto wait_ec = co_await server_open_done.async_wait(5s);
    if (wait_ec) {
        std::cout << "[loopback] server open wait failed: " << wait_ec.message()
                  << "\n";
        co_return 1;
    }
    if (open_ec_client || open_ec_server) {
        std::cout << "[loopback] open failed: client=" << open_ec_client.message()
                  << " server=" << open_ec_server.message() << "\n";
        co_return 1;
    }

    Counters server_counters{};
    secs::core::Event server_done{};
    int server_rc = 0;
    asio::co_spawn(
        ex,
        [&]() -> asio::awaitable<void> {
            server_rc =
                co_await run_server_loop(server, opt, server_counters);
            server_done.set();
        },
        asio::detached);

    const int client_rc = co_await run_client_loop(client, opt);

    server.stop();
    client.stop();
    (void)co_await server.async_wait_reader_stopped(1s);
    (void)co_await client.async_wait_reader_stopped(1s);
    (void)co_await server_done.async_wait(1s);

    std::cout << "[loopback] server: rx_messages=" << server_counters.rx_messages
              << " tx_messages=" << server_counters.tx_messages << "\n";

    if (client_rc == 0 && server_rc == 0) {
        std::cout << "\nPASS\n";
        co_return 0;
    }
    co_return client_rc != 0 ? client_rc : server_rc;
}

asio::awaitable<int> run_server_tcp(const Options &opt) {
    auto ex = co_await asio::this_coro::executor;

    std::error_code parse_ec{};
    const auto addr = asio::ip::make_address(opt.listen_ip, parse_ec);
    if (parse_ec) {
        std::cout << "[server] invalid listen ip: " << parse_ec.message()
                  << "\n";
        co_return 2;
    }

    asio::ip::tcp::acceptor acceptor(ex, asio::ip::tcp::endpoint(addr, opt.port));
    std::cout << "[server] listen: " << opt.listen_ip << ":" << opt.port
              << " session_id=0x" << std::hex << opt.session_id << std::dec
              << " mode=" << (opt.mode == Mode::echo ? "echo" : "ack")
              << " messages=" << opt.messages << "\n";

    auto [acc_ec, socket] =
        co_await acceptor.async_accept(asio::as_tuple(asio::use_awaitable));
    if (acc_ec) {
        std::cout << "[server] accept failed: " << acc_ec.message() << "\n";
        co_return 1;
    }

    Session sess(ex, make_hsms_options(opt.session_id));
    const auto open_ec = co_await sess.async_open_passive(std::move(socket));
    if (open_ec) {
        std::cout << "[server] SELECT failed: " << open_ec.message() << "\n";
        co_return 1;
    }

    Counters counters{};
    const auto rc = co_await run_server_loop(sess, opt, counters);
    sess.stop();
    (void)co_await sess.async_wait_reader_stopped(1s);

    std::cout << "[server] done: rx_messages=" << counters.rx_messages
              << " tx_messages=" << counters.tx_messages << "\n";
    co_return rc;
}

asio::awaitable<int> run_client_tcp(const Options &opt) {
    auto ex = co_await asio::this_coro::executor;

    std::error_code parse_ec{};
    const auto addr = asio::ip::make_address(opt.connect_ip, parse_ec);
    if (parse_ec) {
        std::cout << "[client] invalid connect ip: " << parse_ec.message()
                  << "\n";
        co_return 2;
    }
    asio::ip::tcp::endpoint ep(addr, opt.port);

    std::cout << "[client] connect: " << ep << " session_id=0x" << std::hex
              << opt.session_id << std::dec << "\n";

    Session sess(ex, make_hsms_options(opt.session_id));
    const auto open_ec = co_await sess.async_open_active(ep);
    if (open_ec) {
        std::cout << "[client] SELECT failed: " << open_ec.message() << "\n";
        co_return 1;
    }

    const int rc = co_await run_client_loop(sess, opt);
    sess.stop();
    (void)co_await sess.async_wait_reader_stopped(1s);
    if (rc == 0) {
        std::cout << "\nPASS\n";
    }
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

    asio::signal_set signals(ioc, SIGINT, SIGTERM);
    signals.async_wait([&](const std::error_code &, int) {
        std::cout << "\n[main] 收到退出信号\n";
        ioc.stop();
    });

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            switch (opt->role) {
            case Role::server:
                rc = co_await run_server_tcp(*opt);
                break;
            case Role::client:
                rc = co_await run_client_tcp(*opt);
                break;
            case Role::loopback:
                rc = co_await run_loopback(*opt);
                break;
            }
            ioc.stop();
        },
        asio::detached);

    ioc.run();
    return rc;
}
