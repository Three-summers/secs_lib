/**
 * @file hsms_smlx.cpp
 * @brief HSMS（TCP）示例：SMLX 规则驱动（Rule-Based）
 *
 * 本示例聚焦 “SMLX + RenderContext” 的典型用法：
 * - server/equipment：加载一份 SMLX（`ceid_demo.sml`），收到 S6F11(W=1) 后：
 *   1) match_response_with_capture：按条件规则匹配响应模板，并捕获 $DATAID/$PARAMS；
 *   2) 在捕获上下文上继续注入业务变量（DEVICE_NAME、温度、报警…）；
 *   3) encode_message_body：渲染并编码响应模板，协议层自动回 S6F12。
 * - client/host：使用同一份 SMLX 的 “req_*” 模板生成请求 body，并发送 request。
 *
 * 同时提供三种运行角色（同一份代码，减少示例噪声）：
 * - server：被动端监听 TCP
 * - client：主动端连接 server
 * - loopback：纯内存双工（不依赖 socket），一键跑通端到端
 *
 * 用法：
 *   ./hsms_smlx --role server   --listen 0.0.0.0 --port 5000 --session-id 0x0001 --sml ceid_demo.sml
 *   ./hsms_smlx --role client   --connect 127.0.0.1 --port 5000 --session-id 0x0001 --sml ceid_demo.sml
 *   ./hsms_smlx --role loopback --session-id 0x0001 --sml ceid_demo.sml
 */

#include "secs/core/common.hpp"
#include "secs/core/error.hpp"
#include "secs/core/event.hpp"
#include "secs/hsms/connection.hpp"
#include "secs/hsms/session.hpp"
#include "secs/ii/item.hpp"
#include "secs/protocol/session.hpp"
#include "secs/sml/render.hpp"
#include "secs/sml/runtime.hpp"
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
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
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
using secs::hsms::Session;
using secs::hsms::SessionOptions;
using secs::hsms::Stream;

using secs::ii::Item;
using secs::ii::List;

using secs::protocol::DataMessage;
using secs::protocol::HandlerResult;
using ProtoOptions = secs::protocol::SessionOptions;
using ProtocolSession = secs::protocol::Session;

using secs::sml::MatchFailureReason;
using secs::sml::MatchResponseResult;
using secs::sml::RenderContext;
using secs::sml::Runtime;

enum class Role : std::uint8_t {
    server = 0,
    client = 1,
    loopback = 2,
};

struct Options final {
    Role role{Role::loopback};

    std::string listen_ip{"0.0.0.0"};
    std::string connect_ip{"127.0.0.1"};
    std::uint16_t port{5000};
    std::uint16_t session_id{0x0001};

    std::string sml_path{"ceid_demo.sml"};
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
              << "  --listen <ip>        server 监听地址（默认 0.0.0.0）\n"
              << "  --connect <ip>       client 连接地址（默认 127.0.0.1）\n"
              << "  --port <u16>         端口（默认 5000）\n"
              << "  --session-id <u16>   HSMS data SessionID（支持 0x 前缀，默认 0x0001）\n"
              << "  --sml <path>         SMLX 文件路径（默认 ceid_demo.sml）\n"
              << "  -h, --help           显示帮助\n\n"
              << "示例:\n"
              << "  " << argv0
              << " --role server --listen 0.0.0.0 --port 5000 --session-id 0x0001 --sml ceid_demo.sml\n"
              << "  " << argv0
              << " --role client --connect 127.0.0.1 --port 5000 --session-id 0x0001 --sml ceid_demo.sml\n"
              << "  " << argv0
              << " --role loopback --session-id 0x0001 --sml ceid_demo.sml\n";
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
        if (a == "--listen") {
            const char *v = need_value("--listen");
            if (!v) {
                return std::nullopt;
            }
            opt.listen_ip = v;
            continue;
        }
        if (a == "--connect") {
            const char *v = need_value("--connect");
            if (!v) {
                return std::nullopt;
            }
            opt.connect_ip = v;
            continue;
        }
        if (a == "--port") {
            const char *v = need_value("--port");
            if (!v || !parse_u16(v, opt.port) || opt.port == 0) {
                std::cerr << "非法 port: " << (v ? v : "") << "\n";
                return std::nullopt;
            }
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
        if (a == "--sml") {
            const char *v = need_value("--sml");
            if (!v) {
                return std::nullopt;
            }
            opt.sml_path = v;
            continue;
        }

        std::cerr << "未知参数: " << a << "\n";
        return std::nullopt;
    }

    return opt;
}

static std::optional<std::string> read_file_text(const std::string &path,
                                                 std::string &out_err) {
    std::ifstream f(path, std::ios::in | std::ios::binary);
    if (!f.is_open()) {
        out_err = "open failed";
        return std::nullopt;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

[[nodiscard]] const char *match_reason_name(MatchFailureReason r) noexcept {
    switch (r) {
    case MatchFailureReason::stream_function_mismatch:
        return "stream_function_mismatch";
    case MatchFailureReason::index_out_of_bounds:
        return "index_out_of_bounds";
    case MatchFailureReason::list_index_out_of_bounds:
        return "list_index_out_of_bounds";
    case MatchFailureReason::render_missing_variable:
        return "render_missing_variable";
    case MatchFailureReason::render_type_mismatch:
        return "render_type_mismatch";
    case MatchFailureReason::expected_value_mismatch:
        return "expected_value_mismatch";
    case MatchFailureReason::not_a_list:
        return "not_a_list";
    default:
        return "unknown";
    }
}

static void print_match_traces(const MatchResponseResult &r) {
    if (r.traces.empty()) {
        std::cout << "  (no traces)\n";
        return;
    }

    for (const auto &t : r.traces) {
        std::cout << "  - rule[" << t.rule_index << "] if (" << t.condition_message_name;
        if (t.condition_list_index.has_value()) {
            std::cout << "[" << *t.condition_list_index << "]";
        }
        if (t.condition_index.has_value()) {
            std::cout << "(" << *t.condition_index << ")";
        }
        std::cout << ") failed: " << match_reason_name(t.reason) << " ("
                  << t.detail << ")\n";
    }
}

static void fill_context_for_response(std::string_view response_name,
                                      const DeviceData &d,
                                      RenderContext &ctx) {
    if (response_name == "status_response") {
        ctx.set("DEVICE_NAME", Item::ascii(d.device_name));
        ctx.set("STATUS_CODE", Item::u1({d.status_code}));
        ctx.set("UPTIME_SECONDS", Item::u4({d.uptime_seconds}));
        ctx.set("SVIDS", Item::u2(std::vector<std::uint16_t>{100, 200}));
        ctx.set("BYTES",
                Item::binary(std::vector<secs::ii::byte>{
                    static_cast<secs::ii::byte>(0x02),
                    static_cast<secs::ii::byte>(0x03),
                }));
        ctx.set("BOOLS", Item::boolean(std::vector<bool>{true, false, true}));
        return;
    }

    if (response_name == "temperature_response") {
        ctx.set("TEMP_SENSOR_1", Item::f4({d.temp_sensor_1}));
        ctx.set("TEMP_SENSOR_2", Item::f4({d.temp_sensor_2}));
        ctx.set("TEMP_SENSOR_3", Item::f4({d.temp_sensor_3}));
        const float avg =
            (d.temp_sensor_1 + d.temp_sensor_2 + d.temp_sensor_3) / 3.0f;
        ctx.set("TEMP_AVG", Item::f4({avg}));
        return;
    }

    if (response_name == "alarm_response") {
        ctx.set("ALARM_COUNT", Item::u2({d.alarm_count}));
        ctx.set("ALARM_MSG_1", Item::ascii(d.alarm_msg_1));
        ctx.set("ALARM_MSG_2", Item::ascii(d.alarm_msg_2));
        return;
    }

    if (response_name == "production_response") {
        ctx.set("TOTAL_COUNT", Item::u4({d.total_count}));
        ctx.set("GOOD_COUNT", Item::u4({d.good_count}));
        ctx.set("BAD_COUNT", Item::u4({d.bad_count}));
        const float yield =
            (d.total_count == 0)
                ? 0.0f
                : (static_cast<float>(d.good_count) /
                   static_cast<float>(d.total_count));
        ctx.set("YIELD_RATE", Item::f4({yield}));
        return;
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

struct MemoryChannel final {
    std::deque<byte> buf{};
    bool closed{false};
    secs::core::Event data_event{};
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
                co_return std::pair{
                    std::make_error_code(std::errc::broken_pipe),
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
    asio::any_io_executor ex_;
    std::shared_ptr<MemoryChannel> inbox_;
    std::shared_ptr<MemoryChannel> outbox_;
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

static ProtocolSession make_protocol_session(Session &hsms,
                                             std::uint16_t session_id) {
    ProtoOptions opt{};
    opt.t3 = 3s;
    opt.poll_interval = 10ms;
    return ProtocolSession(hsms, session_id, opt);
}

static asio::awaitable<int> run_client_queries(ProtocolSession &proto,
                                               const Runtime &rt) {
    struct ReqDef final {
        const char *name;
        std::uint16_t ceid;
    };
    const std::array<ReqDef, 4> reqs{{
        {"req_status", 0x1001},
        {"req_temperature", 0x1002},
        {"req_alarm", 0x1003},
        {"req_production", 0x1004},
    }};

    int failures = 0;
    for (std::size_t i = 0; i < reqs.size(); ++i) {
        const std::uint16_t dataid = static_cast<std::uint16_t>(i + 1);
        const auto &r = reqs[i];

        RenderContext ctx;
        ctx.set("DATAID", Item::u2({dataid}));

        std::vector<byte> req_body;
        std::uint8_t stream = 0;
        std::uint8_t function = 0;
        bool w_bit = false;
        const auto enc_ec =
            rt.encode_message_body(r.name, ctx, req_body, &stream, &function, &w_bit);
        if (enc_ec) {
            std::cout << "[client] render request failed: " << enc_ec.message()
                      << "\n";
            ++failures;
            continue;
        }
        if (stream != 6 || function != 11 || !w_bit) {
            std::cout << "[client] request template mismatch: " << r.name << "\n";
            ++failures;
            continue;
        }

        std::cout << "\n[client] request " << r.name << " CEID=0x" << std::hex
                  << r.ceid << std::dec << " DATAID=" << dataid << "\n";

        auto [ec, reply] = co_await proto.async_request(
            stream,
            function,
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

        // 轻量校验：<L <U2 DATAID> <U2 CEID> <...>>
        const auto *list = decoded_opt->get_if<List>();
        if (!list || list->size() < 2) {
            std::cout << "[client] reply body mismatch\n";
            ++failures;
            continue;
        }

        const auto *u2_dataid = (*list)[0].get_if<secs::ii::U2>();
        const auto *u2_ceid = (*list)[1].get_if<secs::ii::U2>();
        if (!u2_dataid || u2_dataid->values.empty() || !u2_ceid ||
            u2_ceid->values.empty()) {
            std::cout << "[client] reply field type mismatch\n";
            ++failures;
            continue;
        }
        if (u2_dataid->values[0] != dataid || u2_ceid->values[0] != r.ceid) {
            std::cout << "[client] reply DATAID/CEID mismatch\n";
            ++failures;
            continue;
        }
    }

    co_return failures == 0 ? 0 : 1;
}

static asio::awaitable<int> run_loopback(std::uint16_t session_id,
                                         const Runtime &rt) {
    auto ex = co_await asio::this_coro::executor;
    auto duplex = make_memory_duplex(ex);

    SessionOptions hsms_opt{};
    hsms_opt.session_id = session_id;
    hsms_opt.t3 = 3s;
    hsms_opt.t5 = 200ms;
    hsms_opt.t6 = 3s;
    hsms_opt.t7 = 3s;
    hsms_opt.t8 = 3s;
    hsms_opt.linktest_interval = 0s;
    hsms_opt.auto_reconnect = false;
    hsms_opt.passive_accept_select = true;

    Session server(ex, hsms_opt);
    Session client(ex, hsms_opt);

    secs::core::Event server_open_done{};
    std::error_code ec_server{};
    asio::co_spawn(
        ex,
        [&]() -> asio::awaitable<void> {
            Connection c(std::move(duplex.server_stream));
            ec_server = co_await server.async_open_passive(std::move(c));
            server_open_done.set();
        },
        asio::detached);

    Connection c(std::move(duplex.client_stream));
    const auto ec_client = co_await client.async_open_active(std::move(c));
    const auto wait_ec = co_await server_open_done.async_wait(5s);
    if (wait_ec) {
        std::cout << "[loopback] server open wait failed: " << wait_ec.message()
                  << "\n";
        co_return 1;
    }
    if (ec_client || ec_server) {
        std::cout << "[loopback] open failed: client=" << ec_client.message()
                  << " server=" << ec_server.message() << "\n";
        co_return 1;
    }

    ProtocolSession server_proto = make_protocol_session(server, session_id);
    ProtocolSession client_proto = make_protocol_session(client, session_id);

    DeviceData device{};

    server_proto.router().set(
        6,
        11,
        [&rt, &device](const DataMessage &req) -> asio::awaitable<HandlerResult> {
            if (!req.w_bit) {
                co_return HandlerResult{std::error_code{}, {}};
            }

            auto [dec_ec, decoded] = secs::utils::decode_one_item(
                bytes_view{req.body.data(), req.body.size()});
            if (dec_ec || !decoded.fully_consumed) {
                co_return HandlerResult{make_error_code(errc::invalid_argument),
                                        {}};
            }

            RenderContext ctx;
            const auto response_name = rt.match_response_with_capture(
                req.stream, req.function, decoded.item, ctx);
            if (!response_name.has_value()) {
                std::cout << "[server] no match\n";
                RenderContext empty;
                const auto traced =
                    rt.match_response_with_trace(req.stream, req.function, decoded.item, empty);
                print_match_traces(traced);
                co_return HandlerResult{make_error_code(errc::invalid_argument),
                                        {}};
            }

            fill_context_for_response(*response_name, device, ctx);

            std::vector<byte> rsp_body;
            std::uint8_t rsp_stream = 0;
            std::uint8_t rsp_function = 0;
            bool rsp_w = false;
            const auto enc_ec = rt.encode_message_body(
                *response_name, ctx, rsp_body, &rsp_stream, &rsp_function, &rsp_w);
            if (enc_ec) {
                co_return HandlerResult{enc_ec, {}};
            }
            const auto expected_function =
                static_cast<std::uint8_t>(req.function + 1u);
            if (rsp_stream != req.stream || rsp_function != expected_function || rsp_w) {
                co_return HandlerResult{make_error_code(errc::invalid_argument), {}};
            }
            co_return HandlerResult{std::error_code{}, std::move(rsp_body)};
        });

    asio::co_spawn(
        ex,
        [&]() -> asio::awaitable<void> { co_await server_proto.async_run(); },
        asio::detached);

    const int rc = co_await run_client_queries(client_proto, rt);

    server_proto.stop();
    client_proto.stop();
    server.stop();
    client.stop();
    (void)co_await server.async_wait_reader_stopped(1s);
    (void)co_await client.async_wait_reader_stopped(1s);

    if (rc == 0) {
        std::cout << "\nPASS\n";
    }
    co_return rc;
}

static asio::awaitable<int> run_server_tcp(const Options &opt,
                                           const Runtime &rt) {
    auto ex = co_await asio::this_coro::executor;

    std::error_code parse_ec{};
    const auto addr = asio::ip::make_address(opt.listen_ip, parse_ec);
    if (parse_ec) {
        std::cout << "[server] invalid listen ip: " << parse_ec.message() << "\n";
        co_return 2;
    }

    asio::ip::tcp::acceptor acceptor(ex, asio::ip::tcp::endpoint(addr, opt.port));

    std::cout << "=== HSMS SMLX Server ===\n\n";
    std::cout << "[server] listen: " << opt.listen_ip << ":" << opt.port
              << " session_id=0x" << std::hex << opt.session_id << std::dec
              << "\n";

    auto [acc_ec, socket] =
        co_await acceptor.async_accept(asio::as_tuple(asio::use_awaitable));
    if (acc_ec) {
        std::cout << "[server] accept failed: " << acc_ec.message() << "\n";
        co_return 1;
    }

    SessionOptions hsms_opt{};
    hsms_opt.session_id = opt.session_id;
    hsms_opt.t3 = 45s;
    hsms_opt.t5 = 1s;
    hsms_opt.t6 = 5s;
    hsms_opt.t7 = 10s;
    hsms_opt.t8 = 5s;
    hsms_opt.linktest_interval = 0s;
    hsms_opt.auto_reconnect = false;
    hsms_opt.passive_accept_select = true;

    Session sess(ex, hsms_opt);
    auto open_ec = co_await sess.async_open_passive(std::move(socket));
    if (open_ec) {
        std::cout << "[server] SELECT failed: " << open_ec.message() << "\n";
        co_return 1;
    }
    std::cout << "[server] selected\n";

    ProtocolSession proto = make_protocol_session(sess, opt.session_id);
    DeviceData device{};

    proto.router().set(
        6,
        11,
        [&rt, &device](const DataMessage &req) -> asio::awaitable<HandlerResult> {
            std::cout << "[server] recv S" << static_cast<int>(req.stream) << "F"
                      << static_cast<int>(req.function) << " W="
                      << (req.w_bit ? 1 : 0) << " body_n=" << req.body.size() << "\n";

            if (!req.w_bit) {
                co_return HandlerResult{std::error_code{}, {}};
            }

            auto [dec_ec, decoded] = secs::utils::decode_one_item(
                bytes_view{req.body.data(), req.body.size()});
            if (dec_ec || !decoded.fully_consumed) {
                co_return HandlerResult{make_error_code(errc::invalid_argument), {}};
            }

            RenderContext ctx;
            const auto response_name =
                rt.match_response_with_capture(req.stream, req.function, decoded.item, ctx);
            if (!response_name.has_value()) {
                std::cout << "[server] no match\n";
                RenderContext empty;
                const auto traced =
                    rt.match_response_with_trace(req.stream, req.function, decoded.item, empty);
                print_match_traces(traced);
                co_return HandlerResult{make_error_code(errc::invalid_argument), {}};
            }

            fill_context_for_response(*response_name, device, ctx);

            std::vector<byte> rsp_body;
            std::uint8_t rsp_stream = 0;
            std::uint8_t rsp_function = 0;
            bool rsp_w = false;
            const auto enc_ec = rt.encode_message_body(
                *response_name, ctx, rsp_body, &rsp_stream, &rsp_function, &rsp_w);
            if (enc_ec) {
                co_return HandlerResult{enc_ec, {}};
            }

            const auto expected_function = static_cast<std::uint8_t>(req.function + 1u);
            if (rsp_stream != req.stream || rsp_function != expected_function || rsp_w) {
                co_return HandlerResult{make_error_code(errc::invalid_argument), {}};
            }

            co_return HandlerResult{std::error_code{}, std::move(rsp_body)};
        });

    co_await proto.async_run();
    co_return 0;
}

static asio::awaitable<int> run_client_tcp(const Options &opt,
                                           const Runtime &rt) {
    auto ex = co_await asio::this_coro::executor;

    std::error_code parse_ec{};
    const auto addr = asio::ip::make_address(opt.connect_ip, parse_ec);
    if (parse_ec) {
        std::cout << "[client] invalid connect ip: " << parse_ec.message() << "\n";
        co_return 2;
    }
    asio::ip::tcp::endpoint ep(addr, opt.port);

    std::cout << "=== HSMS SMLX Client ===\n\n";
    std::cout << "[client] connect: " << ep << " session_id=0x" << std::hex
              << opt.session_id << std::dec << "\n";

    SessionOptions hsms_opt{};
    hsms_opt.session_id = opt.session_id;
    hsms_opt.t3 = 45s;
    hsms_opt.t5 = 1s;
    hsms_opt.t6 = 5s;
    hsms_opt.t7 = 10s;
    hsms_opt.t8 = 5s;
    hsms_opt.linktest_interval = 0s;
    hsms_opt.auto_reconnect = false;
    hsms_opt.passive_accept_select = true;

    Session sess(ex, hsms_opt);
    auto open_ec = co_await sess.async_open_active(ep);
    if (open_ec) {
        std::cout << "[client] SELECT failed: " << open_ec.message() << "\n";
        co_return 1;
    }

    ProtocolSession proto = make_protocol_session(sess, opt.session_id);

    const int rc = co_await run_client_queries(proto, rt);

    proto.stop();
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

    std::string err;
    const auto sml_text_opt = read_file_text(opt->sml_path, err);
    if (!sml_text_opt.has_value()) {
        std::cerr << "ERROR: cannot read SML file: " << opt->sml_path
                  << " (" << err << ")\n";
        return 2;
    }

    Runtime rt;
    if (auto ec = rt.load(*sml_text_opt); ec) {
        std::cerr << "ERROR: failed to load SML: " << ec.message() << "\n";
        return 2;
    }

    std::cout << "[smlx] loaded: " << opt->sml_path << "\n";
    std::cout << "[smlx] messages=" << rt.messages().size()
              << " conditions=" << rt.conditions().size() << "\n\n";

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
                rc = co_await run_server_tcp(*opt, rt);
                break;
            case Role::client:
                rc = co_await run_client_tcp(*opt, rt);
                break;
            case Role::loopback:
                std::cout << "=== HSMS SMLX Loopback ===\n\n";
                rc = co_await run_loopback(opt->session_id, rt);
                break;
            }
            ioc.stop();
        },
        asio::detached);

    ioc.run();
    return rc;
}
