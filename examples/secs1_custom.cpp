/**
 * @file secs1_custom.cpp
 * @brief SECS-I（串口）示例：自定义请求-响应（Code-First）
 *
 * 本示例与 `hsms_custom` 保持同一“业务逻辑”，仅传输层从 HSMS 切换为 SECS-I：
 * - 使用 `secs::secs1::SerialPortLink` + `secs::secs1::StateMachine` 跑 E4 半双工；
 * - 在其上叠加 `secs::protocol::Session`，通过 Router 写 handler 实现 S6F11->S6F12；
 * - client 侧串行发送多条 request，并校验响应。
 *
 * 角色：
 * - server：Equipment 端（默认 reverse_bit=1）
 * - client：Host 端（默认 reverse_bit=0）
 * - loopback：使用 MemoryLink 在进程内模拟“串口线”
 *
 * 用法：
 *   ./secs1_custom --role server   --serial <COMx|/dev/tty*> --baud 9600 --device-id 0x0001
 *   ./secs1_custom --role client   --serial <COMx|/dev/tty*> --baud 9600 --device-id 0x0001
 *   ./secs1_custom --role loopback --device-id 0x0001
 */

#include "secs/core/common.hpp"
#include "secs/core/error.hpp"
#include "secs/ii/item.hpp"
#include "secs/protocol/session.hpp"
#include "secs/secs1/link.hpp"
#include "secs/secs1/serial_port_link.hpp"
#include "secs/secs1/state_machine.hpp"
#include "secs/utils/ii_helpers.hpp"

#include <asio/as_tuple.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/signal_set.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
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
using secs::core::errc;
using secs::core::make_error_code;

using secs::ii::Item;
using secs::ii::List;

using secs::protocol::DataMessage;
using secs::protocol::HandlerResult;
using ProtoOptions = secs::protocol::SessionOptions;
using ProtocolSession = secs::protocol::Session;

enum class Role : std::uint8_t {
    server = 0,
    client = 1,
    loopback = 2,
};

struct Options final {
    Role role{Role::loopback};

    std::string serial{};
    int baud{9600};
    std::uint16_t device_id{0x0001};
    bool reverse_bit{false};
    bool reverse_bit_set{false};
};

struct DeviceData final {
    std::string device_name{"EQUIPMENT-001"};
    std::uint8_t status_code{1};
    std::uint32_t uptime_seconds{12345};

    float temp_sensor_1{25.5f};
    float temp_sensor_2{26.3f};
    float temp_sensor_3{24.8f};

    std::uint16_t alarm_count{2};
    std::string alarm_msg_1{"High temperature warning"};
    std::string alarm_msg_2{"Low pressure alert"};

    std::uint32_t total_count{10000};
    std::uint32_t good_count{9850};
    std::uint32_t bad_count{150};
};

static void print_usage(const char *argv0) {
    std::cout << "用法:\n"
              << "  " << argv0
              << " --role <server|client|loopback> [options]\n\n"
              << "选项:\n"
              << "  --role <server|client|loopback>\n"
              << "  --serial <name>      串口名（Windows: COM5/COM10；Linux: /dev/ttyUSB0）\n"
              << "  --baud <i32>         波特率（默认 9600；虚拟串口可忽略）\n"
              << "  --device-id <u16>    DeviceID（支持 0x 前缀，默认 0x0001）\n"
              << "  --reverse-bit <0|1>  覆盖 R-bit 方向位（默认：server=1, client=0）\n"
              << "  -h, --help           显示帮助\n\n"
              << "示例:\n"
              << "  " << argv0
              << " --role server --serial COM5 --baud 9600 --device-id 0x0001\n"
              << "  " << argv0
              << " --role client --serial COM6 --baud 9600 --device-id 0x0001\n"
              << "  " << argv0
              << " --role loopback --device-id 0x0001\n";
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
        if (a == "--serial") {
            const char *v = need_value("--serial");
            if (!v) {
                return std::nullopt;
            }
            opt.serial = v;
            continue;
        }
        if (a == "--baud") {
            const char *v = need_value("--baud");
            int b = 0;
            if (!v || !parse_i32(v, b) || b <= 0) {
                std::cerr << "非法 baud: " << (v ? v : "") << "\n";
                return std::nullopt;
            }
            opt.baud = b;
            continue;
        }
        if (a == "--device-id") {
            const char *v = need_value("--device-id");
            if (!v || !parse_u16(v, opt.device_id)) {
                std::cerr << "非法 device-id: " << (v ? v : "") << "\n";
                return std::nullopt;
            }
            continue;
        }
        if (a == "--reverse-bit") {
            const char *v = need_value("--reverse-bit");
            int b = 0;
            if (!v || !parse_i32(v, b) || (b != 0 && b != 1)) {
                std::cerr << "非法 reverse-bit: " << (v ? v : "") << "\n";
                return std::nullopt;
            }
            opt.reverse_bit = (b != 0);
            opt.reverse_bit_set = true;
            continue;
        }

        std::cerr << "未知参数: " << a << "\n";
        return std::nullopt;
    }

    // 默认：server=equipment(R=1), client=host(R=0)
    if (!opt.reverse_bit_set) {
        opt.reverse_bit = (opt.role == Role::server);
    }

    if (opt.role != Role::loopback && opt.serial.empty()) {
        std::cerr << "缺少 --serial\n";
        return std::nullopt;
    }

    return opt;
}

[[nodiscard]] std::optional<std::uint16_t>
extract_u2_at(const Item &list_item, std::size_t index) noexcept {
    const auto *list = list_item.get_if<List>();
    if (!list || index >= list->size()) {
        return std::nullopt;
    }
    const auto *u2 = (*list)[index].get_if<secs::ii::U2>();
    if (!u2 || u2->values.empty()) {
        return std::nullopt;
    }
    return u2->values[0];
}

struct CeidFields final {
    std::uint16_t dataid{0};
    std::uint16_t ceid{0};
};

static std::optional<CeidFields> parse_ceid_fields(const Item &decoded) noexcept {
    const auto dataid = extract_u2_at(decoded, 0);
    const auto ceid = extract_u2_at(decoded, 1);
    if (!dataid.has_value() || !ceid.has_value()) {
        return std::nullopt;
    }
    return CeidFields{*dataid, *ceid};
}

static Item build_s6f11_request_item(std::uint16_t dataid, std::uint16_t ceid) {
    return Item::list({
        Item::u2({dataid}),
        Item::u2({ceid}),
        Item::list({}), // PARAMS（示例为空）
    });
}

static Item build_s6f12_response_item(std::uint16_t dataid,
                                      std::uint16_t ceid,
                                      const DeviceData &d) {
    switch (ceid) {
    case 0x1001:
        return Item::list({
            Item::u2({dataid}),
            Item::u2({0x1001}),
            Item::list({
                Item::ascii(d.device_name),
                Item::u1({d.status_code}),
                Item::u4({d.uptime_seconds}),
            }),
        });
    case 0x1002: {
        const float avg =
            (d.temp_sensor_1 + d.temp_sensor_2 + d.temp_sensor_3) / 3.0f;
        return Item::list({
            Item::u2({dataid}),
            Item::u2({0x1002}),
            Item::list({
                Item::ascii("Temperature Sensors"),
                Item::list({
                    Item::f4({d.temp_sensor_1}),
                    Item::f4({d.temp_sensor_2}),
                    Item::f4({d.temp_sensor_3}),
                }),
                Item::f4({avg}),
            }),
        });
    }
    case 0x1003:
        return Item::list({
            Item::u2({dataid}),
            Item::u2({0x1003}),
            Item::list({
                Item::u2({d.alarm_count}),
                Item::list({
                    Item::ascii(d.alarm_msg_1),
                    Item::ascii(d.alarm_msg_2),
                }),
            }),
        });
    case 0x1004: {
        const float yield =
            (d.total_count == 0)
                ? 0.0f
                : (static_cast<float>(d.good_count) /
                   static_cast<float>(d.total_count));
        return Item::list({
            Item::u2({dataid}),
            Item::u2({0x1004}),
            Item::list({
                Item::u4({d.total_count}),
                Item::u4({d.good_count}),
                Item::u4({d.bad_count}),
                Item::f4({yield}),
            }),
        });
    }
    default:
        return Item::list({
            Item::u2({dataid}),
            Item::u2({ceid}),
            Item::list({
                Item::ascii("UNKNOWN_CEID"),
            }),
        });
    }
}

static std::optional<Item> try_decode_body(const DataMessage &msg) {
    if (msg.body.empty()) {
        return Item::list({});
    }
    auto [ec, decoded] =
        secs::utils::decode_one_item(bytes_view{msg.body.data(), msg.body.size()});
    if (ec) {
        return std::nullopt;
    }
    if (!decoded.fully_consumed) {
        return std::nullopt;
    }
    return std::move(decoded.item);
}

static void print_reply_summary(const DataMessage &reply) {
    std::cout << "[client] recv S" << static_cast<int>(reply.stream) << "F"
              << static_cast<int>(reply.function) << " W=" << (reply.w_bit ? 1 : 0)
              << " SB=0x" << std::hex << reply.system_bytes << std::dec
              << " body_n=" << reply.body.size() << "\n";
}

static asio::awaitable<int>
run_client_queries(ProtocolSession &proto) {
    const std::array<std::uint16_t, 4> ceids{
        0x1001, 0x1002, 0x1003, 0x1004,
    };

    int failures = 0;
    for (std::size_t i = 0; i < ceids.size(); ++i) {
        const std::uint16_t dataid = static_cast<std::uint16_t>(i + 1);
        const std::uint16_t ceid = ceids[i];

        const auto req_item = build_s6f11_request_item(dataid, ceid);
        auto [enc_ec, req_body] = secs::utils::encode_item(req_item);
        if (enc_ec) {
            std::cout << "[client] encode request failed: " << enc_ec.message() << "\n";
            ++failures;
            continue;
        }

        std::cout << "\n[client] request S6F11 CEID=0x" << std::hex << ceid
                  << std::dec << " DATAID=" << dataid << "\n";

        auto [ec, reply] = co_await proto.async_request(
            6,
            11,
            bytes_view{req_body.data(), req_body.size()},
            3s);
        if (ec) {
            std::cout << "[client] request failed: " << ec.message() << "\n";
            ++failures;
            continue;
        }

        print_reply_summary(reply);

        if (reply.stream != 6 || reply.function != 12 || reply.w_bit) {
            std::cout << "[client] reply header mismatch\n";
            ++failures;
            continue;
        }

        const auto decoded_opt = try_decode_body(reply);
        if (!decoded_opt.has_value()) {
            std::cout << "[client] decode reply failed\n";
            ++failures;
            continue;
        }

        const auto fields = parse_ceid_fields(*decoded_opt);
        if (!fields.has_value() || fields->dataid != dataid || fields->ceid != ceid) {
            std::cout << "[client] reply DATAID/CEID mismatch\n";
            ++failures;
        }
    }

    co_return failures == 0 ? 0 : 1;
}

static ProtocolSession make_protocol_session(secs::secs1::StateMachine &sm,
                                             std::uint16_t device_id,
                                             bool reverse_bit) {
    secs::protocol::SessionOptions opt{};
    opt.t3 = 3s;
    opt.poll_interval = 10ms;
    opt.secs1_reverse_bit = reverse_bit;
    return ProtocolSession(sm, device_id, opt);
}

static asio::awaitable<int> run_loopback(std::uint16_t device_id) {
    auto ex = co_await asio::this_coro::executor;

    auto [host_link, eq_link] = secs::secs1::MemoryLink::create(ex);

    secs::secs1::StateMachine host_sm(host_link, device_id);
    secs::secs1::StateMachine eq_sm(eq_link, device_id);

    ProtocolSession host = make_protocol_session(host_sm, device_id, false);
    ProtocolSession eq = make_protocol_session(eq_sm, device_id, true);

    DeviceData device{};
    eq.router().set(
        6,
        11,
        [&device](const DataMessage &req) -> asio::awaitable<HandlerResult> {
            if (!req.w_bit) {
                co_return HandlerResult{std::error_code{}, {}};
            }
            auto [dec_ec, decoded] = secs::utils::decode_one_item_if_any(
                bytes_view{req.body.data(), req.body.size()});
            if (dec_ec || !decoded.has_value()) {
                co_return HandlerResult{make_error_code(errc::invalid_argument), {}};
            }
            const auto parsed = parse_ceid_fields(decoded->item);
            if (!parsed.has_value()) {
                co_return HandlerResult{make_error_code(errc::invalid_argument), {}};
            }
            const auto rsp_item = build_s6f12_response_item(
                parsed->dataid, parsed->ceid, device);
            auto [enc_ec, body] = secs::utils::encode_item(rsp_item);
            if (enc_ec) {
                co_return HandlerResult{enc_ec, {}};
            }
            co_return HandlerResult{std::error_code{}, std::move(body)};
        });

    asio::co_spawn(
        ex,
        [&]() -> asio::awaitable<void> { co_await eq.async_run(); },
        asio::detached);

    const int rc = co_await run_client_queries(host);

    host.stop();
    eq.stop();
    if (rc == 0) {
        std::cout << "\nPASS\n";
    }
    co_return rc;
}

static asio::awaitable<int> run_server_serial(const Options &opt) {
    auto ex = co_await asio::this_coro::executor;

    std::cout << "=== SECS-I Custom Server (Equipment) ===\n\n";
    std::cout << "[server] serial=" << opt.serial << " baud=" << opt.baud
              << " device_id=0x" << std::hex << opt.device_id << std::dec
              << " reverse_bit=" << (opt.reverse_bit ? 1 : 0) << "\n";

    auto [open_ec, link] = secs::secs1::SerialPortLink::open(ex, opt.serial, opt.baud);
    if (open_ec) {
        std::cout << "[server] open serial failed: " << open_ec.message() << "\n";
        co_return 1;
    }

    secs::secs1::StateMachine sm(link, opt.device_id);
    ProtocolSession proto = make_protocol_session(sm, opt.device_id, opt.reverse_bit);

    DeviceData device{};
    proto.router().set(
        6,
        11,
        [&device](const DataMessage &req) -> asio::awaitable<HandlerResult> {
            std::cout << "[server] recv S" << static_cast<int>(req.stream) << "F"
                      << static_cast<int>(req.function) << " W="
                      << (req.w_bit ? 1 : 0) << " body_n=" << req.body.size() << "\n";
            if (!req.w_bit) {
                co_return HandlerResult{std::error_code{}, {}};
            }
            auto [dec_ec, decoded] = secs::utils::decode_one_item_if_any(
                bytes_view{req.body.data(), req.body.size()});
            if (dec_ec || !decoded.has_value()) {
                co_return HandlerResult{make_error_code(errc::invalid_argument), {}};
            }
            const auto parsed = parse_ceid_fields(decoded->item);
            if (!parsed.has_value()) {
                co_return HandlerResult{make_error_code(errc::invalid_argument), {}};
            }
            const auto rsp_item = build_s6f12_response_item(
                parsed->dataid, parsed->ceid, device);
            auto [enc_ec, body] = secs::utils::encode_item(rsp_item);
            if (enc_ec) {
                co_return HandlerResult{enc_ec, {}};
            }
            co_return HandlerResult{std::error_code{}, std::move(body)};
        });

    co_await proto.async_run();
    co_return 0;
}

static asio::awaitable<int> run_client_serial(const Options &opt) {
    auto ex = co_await asio::this_coro::executor;

    std::cout << "=== SECS-I Custom Client (Host) ===\n\n";
    std::cout << "[client] serial=" << opt.serial << " baud=" << opt.baud
              << " device_id=0x" << std::hex << opt.device_id << std::dec
              << " reverse_bit=" << (opt.reverse_bit ? 1 : 0) << "\n";

    auto [open_ec, link] = secs::secs1::SerialPortLink::open(ex, opt.serial, opt.baud);
    if (open_ec) {
        std::cout << "[client] open serial failed: " << open_ec.message() << "\n";
        co_return 1;
    }

    secs::secs1::StateMachine sm(link, opt.device_id);
    ProtocolSession proto = make_protocol_session(sm, opt.device_id, opt.reverse_bit);

    const int rc = co_await run_client_queries(proto);
    proto.stop();
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
                rc = co_await run_server_serial(*opt);
                break;
            case Role::client:
                rc = co_await run_client_serial(*opt);
                break;
            case Role::loopback:
                std::cout << "=== SECS-I Custom Loopback ===\n\n";
                rc = co_await run_loopback(opt->device_id);
                break;
            }
            ioc.stop();
        },
        asio::detached);

    ioc.run();
    return rc;
}
