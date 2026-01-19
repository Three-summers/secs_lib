/**
 * @file hsms_custom.cpp
 * @brief HSMS（TCP）示例：自定义请求-响应（Code-First）
 *
 * 本示例聚焦“自定义请求-响应”的写法：
 * - 使用 `secs::protocol::Session` 统一收发（上层只关心 SxFy + body bytes）；
 * - 通过 Router 注册 handler：收到 S6F11(W=1) 后返回响应 body，协议层自动回 S6F12。
 *
 * 同时提供三种运行角色（同一份代码，减少示例噪声）：
 * - server：被动端监听 TCP，等待对端连接并按规则回包
 * - client：主动端连接 server，发送多条 CEID 查询请求并校验响应
 * - loopback：纯内存双工（不依赖 socket），一键跑通端到端
 *
 * 用法：
 *   ./hsms_custom --role server   --listen 0.0.0.0 --port 5000 --session-id 0x0001
 *   ./hsms_custom --role client   --connect 127.0.0.1 --port 5000 --session-id 0x0001
 *   ./hsms_custom --role loopback
 */

#include "secs/core/common.hpp"
#include "secs/core/error.hpp"
#include "secs/core/event.hpp"
#include "secs/hsms/connection.hpp"
#include "secs/hsms/session.hpp"
#include "secs/ii/item.hpp"
#include "secs/protocol/session.hpp"
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
              << "  -h, --help           显示帮助\n\n"
              << "示例:\n"
              << "  " << argv0
              << " --role server --listen 0.0.0.0 --port 5000 --session-id 0x0001\n"
              << "  " << argv0
              << " --role client --connect 127.0.0.1 --port 5000 --session-id 0x0001\n"
              << "  " << argv0 << " --role loopback\n";
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

        std::cerr << "未知参数: " << a << "\n";
        return std::nullopt;
    }

    return opt;
}

struct CeidRequest final {
    std::uint16_t dataid{0};
    std::uint16_t ceid{0};
};

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

[[nodiscard]] std::optional<CeidRequest>
parse_s6f11_request(const Item &decoded) noexcept {
    const auto dataid = extract_u2_at(decoded, 0);
    const auto ceid = extract_u2_at(decoded, 1);
    if (!dataid.has_value() || !ceid.has_value()) {
        return std::nullopt;
    }
    return CeidRequest{*dataid, *ceid};
}

[[nodiscard]] Item build_s6f11_request_item(std::uint16_t dataid,
                                            std::uint16_t ceid) {
    return Item::list({
        Item::u2({dataid}),
        Item::u2({ceid}),
        Item::list({}), // PARAMS（示例为空）
    });
}

[[nodiscard]] Item build_s6f12_response_item(std::uint16_t dataid,
                                             std::uint16_t ceid,
                                             const DeviceData &d) {
    switch (ceid) {
    case 0x1001: {
        return Item::list({
            Item::u2({dataid}),
            Item::u2({0x1001}),
            Item::list({
                Item::ascii(d.device_name),
                Item::u1({d.status_code}),
                Item::u4({d.uptime_seconds}),
            }),
        });
    }
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
    case 0x1003: {
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
    }
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
        // 未知 CEID：返回一个最小响应，便于联调观察（也可改为 reject/不回包）。
        return Item::list({
            Item::u2({dataid}),
            Item::u2({ceid}),
            Item::list({
                Item::ascii("UNKNOWN_CEID"),
            }),
        });
    }
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

static asio::awaitable<int> run_client_queries(ProtocolSession &proto) {
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
            std::cout << "[client] encode request failed: " << enc_ec.message()
                      << "\n";
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

        const auto parsed = parse_s6f11_request(*decoded_opt);
        if (!parsed.has_value() || parsed->dataid != dataid) {
            std::cout << "[client] reply DATAID mismatch\n";
            ++failures;
        }
    }

    co_return failures == 0 ? 0 : 1;
}

static ProtocolSession make_protocol_session(Session &hsms,
                                             std::uint16_t session_id) {
    ProtoOptions opt{};
    opt.t3 = 3s;
    opt.poll_interval = 10ms;
    return ProtocolSession(hsms, session_id, opt);
}

static asio::awaitable<int> run_loopback(std::uint16_t session_id) {
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

    // 注意：asio::co_spawn(use_awaitable) 返回的 awaitable 可能是“惰性”的；
    // 这里用 Event 显式并行启动 server open，避免 client 先发 SELECT 但 server 还没开始读。
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
        [&device](const DataMessage &req) -> asio::awaitable<HandlerResult> {
            // W=0：协议层不会回包；这里仍返回 OK，避免多余噪声。
            if (!req.w_bit) {
                co_return HandlerResult{std::error_code{}, {}};
            }

            auto [dec_ec, decoded] = secs::utils::decode_one_item_if_any(
                bytes_view{req.body.data(), req.body.size()});
            if (dec_ec || !decoded.has_value()) {
                co_return HandlerResult{make_error_code(errc::invalid_argument),
                                        {}};
            }

            const auto parsed = parse_s6f11_request(decoded->item);
            if (!parsed.has_value()) {
                co_return HandlerResult{make_error_code(errc::invalid_argument),
                                        {}};
            }

            const auto rsp_item =
                build_s6f12_response_item(parsed->dataid, parsed->ceid, device);
            auto [enc_ec, body] = secs::utils::encode_item(rsp_item);
            if (enc_ec) {
                co_return HandlerResult{enc_ec, {}};
            }
            co_return HandlerResult{std::error_code{}, std::move(body)};
        });

    // server 侧启动接收循环（负责路由与自动回包）。
    // 注意：use_awaitable 形式可能是“惰性”的；这里使用 detached 确保立即启动。
    asio::co_spawn(
        ex,
        [&]() -> asio::awaitable<void> { co_await server_proto.async_run(); },
        asio::detached);

    const int rc = co_await run_client_queries(client_proto);

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

static asio::awaitable<int> run_server_tcp(const Options &opt) {
    auto ex = co_await asio::this_coro::executor;

    std::error_code parse_ec{};
    const auto addr = asio::ip::make_address(opt.listen_ip, parse_ec);
    if (parse_ec) {
        std::cout << "[server] invalid listen ip: " << parse_ec.message()
                  << "\n";
        co_return 2;
    }

    asio::ip::tcp::acceptor acceptor(
        ex,
        asio::ip::tcp::endpoint(addr, opt.port));

    std::cout << "=== HSMS Custom Server ===\n\n";
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
        [&device](const DataMessage &req) -> asio::awaitable<HandlerResult> {
            std::cout << "[server] recv S" << static_cast<int>(req.stream) << "F"
                      << static_cast<int>(req.function) << " W="
                      << (req.w_bit ? 1 : 0) << " body_n=" << req.body.size()
                      << "\n";

            if (!req.w_bit) {
                co_return HandlerResult{std::error_code{}, {}};
            }

            auto [dec_ec, decoded] = secs::utils::decode_one_item_if_any(
                bytes_view{req.body.data(), req.body.size()});
            if (dec_ec || !decoded.has_value()) {
                co_return HandlerResult{make_error_code(errc::invalid_argument),
                                        {}};
            }

            const auto parsed = parse_s6f11_request(decoded->item);
            if (!parsed.has_value()) {
                co_return HandlerResult{make_error_code(errc::invalid_argument),
                                        {}};
            }

            const auto rsp_item =
                build_s6f12_response_item(parsed->dataid, parsed->ceid, device);
            auto [enc_ec, body] = secs::utils::encode_item(rsp_item);
            if (enc_ec) {
                co_return HandlerResult{enc_ec, {}};
            }
            co_return HandlerResult{std::error_code{}, std::move(body)};
        });

    co_await proto.async_run();
    co_return 0;
}

static asio::awaitable<int> run_client_tcp(const Options &opt) {
    auto ex = co_await asio::this_coro::executor;

    std::error_code parse_ec{};
    const auto addr = asio::ip::make_address(opt.connect_ip, parse_ec);
    if (parse_ec) {
        std::cout << "[client] invalid connect ip: " << parse_ec.message()
                  << "\n";
        co_return 2;
    }
    asio::ip::tcp::endpoint ep(addr, opt.port);

    std::cout << "=== HSMS Custom Client ===\n\n";
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

    const int rc = co_await run_client_queries(proto);
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
                std::cout << "=== HSMS Custom Loopback ===\n\n";
                rc = co_await run_loopback(opt->session_id);
                break;
            }
            ioc.stop();
        },
        asio::detached);

    ioc.run();
    return rc;
}
