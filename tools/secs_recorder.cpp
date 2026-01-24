#include "secs/hsms/connection.hpp"
#include "secs/protocol/router.hpp"
#include "secs/tools/recording.hpp"

#include <asio/as_tuple.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/signal_set.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

#include <atomic>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

struct Options final {
    std::string listen;
    std::string forward;
    std::string output;
    std::optional<std::uint16_t> session_id_filter{};
    bool include_sml_header{false};
    bool include_peer_local{false};

    enum class DirFilter : std::uint8_t { any = 0, tx = 1, rx = 2 };
    std::vector<std::pair<std::uint8_t, std::uint8_t>> only_sf{};
    std::vector<std::uint8_t> only_streams{};
    DirFilter dir{DirFilter::any};
    std::optional<std::uint64_t> since_us{};
    std::optional<std::uint64_t> until_us{};
    std::optional<std::size_t> limit{};
};

static void print_usage(const char *argv0) {
    std::cout << "用法:\n"
              << "  " << argv0
              << " --listen <ip:port> --forward <ip:port> --output <file.jsonl>\n\n"
              << "说明:\n"
              << "  - 作为 HSMS TCP 透明代理：监听 --listen，转发到 --forward\n"
              << "  - 仅录制 data message（SType=0），输出为 JSON Lines（每行一条）\n"
              << "  - 方向约定：客户端 -> 转发端 记为 TX，反向记为 RX\n\n"
              << "选项:\n"
              << "  --listen <ip:port>    监听地址（例如 0.0.0.0:5000）\n"
              << "  --forward <ip:port>   转发目标（例如 192.168.1.100:5000）\n"
              << "  --output <path>       输出文件路径（覆盖写）\n"
              << "  --session-id <u16>    仅录制该 SessionID 的 data message（可选，支持 0x 前缀）\n"
              << "  --include-sml         额外写入 \"sml\" 字段（仅头部：SxFy[ W].）\n"
              << "  --include-peer-local  额外写入 \"peer\"/\"local\" 字段（HSMS proxy 场景）\n"
              << "  --only s=<u8>,f=<u8>  仅录制指定 S/F（可重复指定）\n"
              << "  --only-stream <u8>    仅录制指定 Stream（可重复指定）\n"
              << "  --dir <tx|rx>         仅录制指定方向（默认不过滤）\n"
              << "  --since-us <u64>      仅录制启动后 ts_us >= since-us 的消息\n"
              << "  --until-us <u64>      仅录制启动后 ts_us <= until-us 的消息（超过后自动停止）\n"
              << "  --limit <N>           最多录制 N 条（按过滤后的消息计数）\n"
              << "  -h, --help            显示帮助\n";
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

static bool parse_u64(std::string_view s, std::uint64_t &out) {
    std::uint64_t v = 0;
    int base = 10;
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s.remove_prefix(2);
    }
    auto *begin = s.data();
    auto *end = s.data() + s.size();
    auto [ptr, ec] = std::from_chars(begin, end, v, base);
    if (ec != std::errc{} || ptr != end) {
        return false;
    }
    out = v;
    return true;
}

static bool parse_size(std::string_view s, std::size_t &out) {
    std::uint64_t v = 0;
    if (!parse_u64(s, v) || v == 0) {
        return false;
    }
    if (v > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        return false;
    }
    out = static_cast<std::size_t>(v);
    return true;
}

static std::string_view trim(std::string_view s) {
    while (!s.empty() &&
           std::isspace(static_cast<unsigned char>(s.front())) != 0) {
        s.remove_prefix(1);
    }
    while (!s.empty() &&
           std::isspace(static_cast<unsigned char>(s.back())) != 0) {
        s.remove_suffix(1);
    }
    return s;
}

static bool parse_only_sf(std::string_view s,
                          std::uint8_t &out_stream,
                          std::uint8_t &out_function) {
    std::optional<std::uint8_t> stream{};
    std::optional<std::uint8_t> function{};

    for (;;) {
        const auto comma = s.find(',');
        auto part = (comma == std::string_view::npos) ? s : s.substr(0, comma);
        part = trim(part);
        if (part.empty()) {
            return false;
        }
        const auto eq = part.find('=');
        if (eq == std::string_view::npos) {
            return false;
        }
        const auto key = trim(part.substr(0, eq));
        const auto val = trim(part.substr(eq + 1));
        if (key == "s" || key == "stream") {
            std::uint8_t v = 0;
            if (!parse_u8(val, v)) {
                return false;
            }
            stream = v;
        } else if (key == "f" || key == "function") {
            std::uint8_t v = 0;
            if (!parse_u8(val, v)) {
                return false;
            }
            function = v;
        } else {
            return false;
        }

        if (comma == std::string_view::npos) {
            break;
        }
        s.remove_prefix(comma + 1);
    }

    if (!stream.has_value() || !function.has_value()) {
        return false;
    }
    out_stream = *stream;
    out_function = *function;
    return true;
}

static bool parse_dir(std::string_view s, Options::DirFilter &out) {
    if (s == "tx" || s == "TX") {
        out = Options::DirFilter::tx;
        return true;
    }
    if (s == "rx" || s == "RX") {
        out = Options::DirFilter::rx;
        return true;
    }
    return false;
}

static bool parse_endpoint(std::string_view text,
                           asio::ip::tcp::endpoint &out) {
    std::string host;
    std::string_view port_sv;

    if (!text.empty() && text.front() == '[') {
        // IPv6: [::1]:5000
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
        if (a == "--listen") {
            const char *v = need_value("--listen");
            if (!v) {
                return std::nullopt;
            }
            opt.listen = v;
            continue;
        }
        if (a == "--forward") {
            const char *v = need_value("--forward");
            if (!v) {
                return std::nullopt;
            }
            opt.forward = v;
            continue;
        }
        if (a == "--output") {
            const char *v = need_value("--output");
            if (!v) {
                return std::nullopt;
            }
            opt.output = v;
            continue;
        }
        if (a == "--session-id") {
            const char *v = need_value("--session-id");
            if (!v) {
                return std::nullopt;
            }
            std::uint16_t sid = 0;
            if (!parse_u16(v, sid)) {
                std::cerr << "非法 session-id: " << v << "\n";
                return std::nullopt;
            }
            opt.session_id_filter = sid;
            continue;
        }
        if (a == "--include-sml") {
            opt.include_sml_header = true;
            continue;
        }
        if (a == "--include-peer-local") {
            opt.include_peer_local = true;
            continue;
        }
        if (a == "--only") {
            const char *v = need_value("--only");
            if (!v) {
                return std::nullopt;
            }
            std::uint8_t s8 = 0;
            std::uint8_t f8 = 0;
            if (!parse_only_sf(v, s8, f8)) {
                std::cerr << "非法 only: " << v << "（期望 s=<u8>,f=<u8>）\n";
                return std::nullopt;
            }
            opt.only_sf.emplace_back(s8, f8);
            continue;
        }
        if (a == "--only-stream") {
            const char *v = need_value("--only-stream");
            if (!v) {
                return std::nullopt;
            }
            std::uint8_t s8 = 0;
            if (!parse_u8(v, s8)) {
                std::cerr << "非法 only-stream: " << v << "\n";
                return std::nullopt;
            }
            opt.only_streams.push_back(s8);
            continue;
        }
        if (a == "--dir") {
            const char *v = need_value("--dir");
            if (!v) {
                return std::nullopt;
            }
            if (!parse_dir(v, opt.dir)) {
                std::cerr << "非法 dir: " << v << "（期望 tx|rx）\n";
                return std::nullopt;
            }
            continue;
        }
        if (a == "--since-us") {
            const char *v = need_value("--since-us");
            if (!v) {
                return std::nullopt;
            }
            std::uint64_t u = 0;
            if (!parse_u64(v, u)) {
                std::cerr << "非法 since-us: " << v << "\n";
                return std::nullopt;
            }
            opt.since_us = u;
            continue;
        }
        if (a == "--until-us") {
            const char *v = need_value("--until-us");
            if (!v) {
                return std::nullopt;
            }
            std::uint64_t u = 0;
            if (!parse_u64(v, u)) {
                std::cerr << "非法 until-us: " << v << "\n";
                return std::nullopt;
            }
            opt.until_us = u;
            continue;
        }
        if (a == "--limit") {
            const char *v = need_value("--limit");
            if (!v) {
                return std::nullopt;
            }
            std::size_t n = 0;
            if (!parse_size(v, n)) {
                std::cerr << "非法 limit: " << v << "（期望 N>=1）\n";
                return std::nullopt;
            }
            opt.limit = n;
            continue;
        }

        std::cerr << "未知参数: " << a << "\n";
        return std::nullopt;
    }

    if (opt.listen.empty() || opt.forward.empty() || opt.output.empty()) {
        return std::nullopt;
    }
    if (opt.since_us.has_value() && opt.until_us.has_value() &&
        *opt.since_us > *opt.until_us) {
        std::cerr << "非法时间窗：since-us > until-us\n";
        return std::nullopt;
    }
    return opt;
}

static std::uint64_t now_rel_us(std::chrono::steady_clock::time_point start) {
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - start)
                        .count();
    return (us < 0) ? 0u : static_cast<std::uint64_t>(us);
}

static bool matches_sf_selector(
    const std::vector<std::pair<std::uint8_t, std::uint8_t>> &only_sf,
    const std::vector<std::uint8_t> &only_streams,
    std::uint8_t stream,
    std::uint8_t function) {
    if (only_sf.empty() && only_streams.empty()) {
        return true;
    }
    for (const auto s : only_streams) {
        if (stream == s) {
            return true;
        }
    }
    for (const auto &sf : only_sf) {
        if (stream == sf.first && function == sf.second) {
            return true;
        }
    }
    return false;
}

static void json_escape_to(std::ostream &os, std::string_view s) {
    for (const unsigned char c : s) {
        switch (c) {
        case '\\':
            os << "\\\\";
            break;
        case '"':
            os << "\\\"";
            break;
        case '\b':
            os << "\\b";
            break;
        case '\f':
            os << "\\f";
            break;
        case '\n':
            os << "\\n";
            break;
        case '\r':
            os << "\\r";
            break;
        case '\t':
            os << "\\t";
            break;
        default:
            if (c < 0x20) {
                static constexpr char kHex[] = "0123456789abcdef";
                os << "\\u00" << kHex[(c >> 4) & 0x0F] << kHex[c & 0x0F];
            } else {
                os << static_cast<char>(c);
            }
            break;
        }
    }
}

static void json_string(std::ostream &os, std::string_view s) {
    os << '"';
    json_escape_to(os, s);
    os << '"';
}

static std::string endpoint_to_string(const asio::ip::tcp::endpoint &ep) {
    std::ostringstream os;
    const auto addr = ep.address().to_string();
    if (ep.address().is_v6()) {
        os << '[' << addr << "]:" << ep.port();
    } else {
        os << addr << ':' << ep.port();
    }
    return os.str();
}

static std::error_code rewrite_jsonl_add_peer_local_fields(
    const std::string &path,
    std::string_view tx_peer,
    std::string_view tx_local,
    std::string_view rx_peer,
    std::string_view rx_local) {
    const auto suffix =
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const std::filesystem::path p(path);
    const std::filesystem::path tmp(p.string() + ".tmp." + suffix);
    const std::filesystem::path bak(p.string() + ".bak." + suffix);

    std::ifstream in(path, std::ios::in | std::ios::binary);
    if (!in.is_open()) {
        return std::make_error_code(std::errc::no_such_file_or_directory);
    }
    std::ofstream out(tmp, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        return std::make_error_code(std::errc::io_error);
    }

    std::string line;
    while (std::getline(in, line)) {
        if (line.find("\"peer\":") != std::string::npos ||
            line.find("\"local\":") != std::string::npos) {
            out << line << '\n';
            continue;
        }

        const bool is_tx =
            line.find("\"dir\":\"TX\"") != std::string::npos;
        const bool is_rx =
            line.find("\"dir\":\"RX\"") != std::string::npos;
        if (!is_tx && !is_rx) {
            out << line << '\n';
            continue;
        }

        const auto peer = is_tx ? tx_peer : rx_peer;
        const auto local = is_tx ? tx_local : rx_local;
        if (peer.empty() || local.empty()) {
            out << line << '\n';
            continue;
        }

        const auto rb = line.rfind('}');
        if (rb == std::string::npos) {
            out << line << '\n';
            continue;
        }

        out << std::string_view{line.data(), rb};
        out << ",\"peer\":";
        json_string(out, peer);
        out << ",\"local\":";
        json_string(out, local);
        out << "}\n";
    }

    if (!out) {
        return std::make_error_code(std::errc::io_error);
    }

    std::error_code ec{};
    std::filesystem::rename(p, bak, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return ec;
    }
    std::filesystem::rename(tmp, p, ec);
    if (ec) {
        std::error_code restore_ec{};
        std::filesystem::rename(bak, p, restore_ec);
        std::filesystem::remove(tmp, restore_ec);
        return ec;
    }
    std::filesystem::remove(bak, ec);
    return {};
}

static asio::awaitable<void> pump(secs::hsms::Connection &in,
                                  secs::hsms::Connection &out,
                                  secs::tools::MessageRecorder &recorder,
                                  bool is_tx,
                                  std::optional<std::uint16_t> session_id_filter,
                                  const Options &opt,
                                  std::chrono::steady_clock::time_point start,
                                  std::atomic<std::size_t> &recorded_count) {
    for (;;) {
        auto [rd_ec, msg] =
            co_await in.async_read_message();
        if (rd_ec) {
            out.cancel_and_close();
            co_return;
        }

        bool stop_after_this = false;
        const auto cur_us = now_rel_us(start);
        if (opt.until_us.has_value() && cur_us > *opt.until_us) {
            stop_after_this = true;
        }

        if (msg.is_data()) {
            if (!session_id_filter.has_value() ||
                msg.header.session_id == *session_id_filter) {
                const auto s = msg.stream();
                const auto f = msg.function();
                const bool dir_ok =
                    (opt.dir == Options::DirFilter::any) ||
                    (opt.dir == Options::DirFilter::tx && is_tx) ||
                    (opt.dir == Options::DirFilter::rx && !is_tx);
                const bool sf_ok = matches_sf_selector(
                    opt.only_sf, opt.only_streams, s, f);
                const bool time_ok =
                    (!opt.since_us.has_value() || cur_us >= *opt.since_us) &&
                    (!opt.until_us.has_value() || cur_us <= *opt.until_us);

                const bool under_limit =
                    !opt.limit.has_value() ||
                    recorded_count.load(std::memory_order_relaxed) <
                        *opt.limit;

                const bool should_record =
                    dir_ok && sf_ok && time_ok && under_limit;
                if (should_record) {
                    secs::protocol::DataMessage dm{};
                    dm.stream = s;
                    dm.function = f;
                    dm.w_bit = msg.w_bit();
                    dm.system_bytes = msg.header.system_bytes;
                    dm.body = msg.body; // 录制需要保留一份副本（转发仍要使用 msg）
                    if (is_tx) {
                        recorder.record_tx(dm);
                    } else {
                        recorder.record_rx(dm);
                    }
                    const auto n =
                        recorded_count.fetch_add(1,
                                                 std::memory_order_relaxed) +
                        1;
                    if (opt.limit.has_value() && n >= *opt.limit) {
                        stop_after_this = true;
                    }
                }
            }
        }

        const auto wr_ec = co_await out.async_write_message(msg);
        if (wr_ec) {
            in.cancel_and_close();
            co_return;
        }
        if (stop_after_this) {
            in.cancel_and_close();
            out.cancel_and_close();
            co_return;
        }
    }
}

static asio::awaitable<int> run(const Options &opt) {
    auto ex = co_await asio::this_coro::executor;

    asio::ip::tcp::endpoint listen_ep;
    asio::ip::tcp::endpoint forward_ep;
    if (!parse_endpoint(opt.listen, listen_ep)) {
        std::cout << "非法 listen: " << opt.listen << "\n";
        co_return 2;
    }
    if (!parse_endpoint(opt.forward, forward_ep)) {
        std::cout << "非法 forward: " << opt.forward << "\n";
        co_return 2;
    }

    const auto start = std::chrono::steady_clock::now();
    auto recorder = std::make_unique<secs::tools::MessageRecorder>(opt.output);
    if (!recorder->is_open()) {
        std::cout << "打开输出文件失败: " << opt.output << " ec="
                  << recorder->last_error().message() << "\n";
        co_return 2;
    }
    recorder->set_include_sml_header(opt.include_sml_header);

    asio::ip::tcp::acceptor acceptor(ex, listen_ep);
    std::cout << "[recorder] listen: " << listen_ep << "\n";
    std::cout << "[recorder] forward: " << forward_ep << "\n";
    std::cout << "[recorder] output: " << opt.output << "\n";
    if (opt.session_id_filter.has_value()) {
        std::cout << "[recorder] session-id filter: 0x" << std::hex
                  << *opt.session_id_filter << std::dec << "\n";
    }
    if (opt.limit.has_value()) {
        std::cout << "[recorder] limit: " << *opt.limit << "\n";
    }

    auto [acc_ec, client_socket] =
        co_await acceptor.async_accept(asio::as_tuple(asio::use_awaitable));
    if (acc_ec) {
        std::cout << "[recorder] accept failed: " << acc_ec.message() << "\n";
        co_return 1;
    }
    std::cout << "[recorder] client connected\n";
    std::string tx_peer;
    std::string tx_local;
    {
        std::error_code ec{};
        const auto peer_ep = client_socket.remote_endpoint(ec);
        if (!ec) {
            tx_peer = endpoint_to_string(peer_ep);
        }
        ec.clear();
        const auto local_ep = client_socket.local_endpoint(ec);
        if (!ec) {
            tx_local = endpoint_to_string(local_ep);
        }
    }

    asio::ip::tcp::socket server_socket(ex);
    auto [conn_ec] = co_await server_socket.async_connect(
        forward_ep, asio::as_tuple(asio::use_awaitable));
    if (conn_ec) {
        std::cout << "[recorder] connect forward failed: " << conn_ec.message()
                  << "\n";
        co_return 1;
    }

    std::string rx_peer;
    std::string rx_local;
    {
        std::error_code ec{};
        const auto peer_ep = server_socket.remote_endpoint(ec);
        if (!ec) {
            rx_peer = endpoint_to_string(peer_ep);
        }
        ec.clear();
        const auto local_ep = server_socket.local_endpoint(ec);
        if (!ec) {
            rx_local = endpoint_to_string(local_ep);
        }
    }

    secs::hsms::Connection client_conn(std::move(client_socket));
    secs::hsms::Connection server_conn(std::move(server_socket));
    std::cout << "[recorder] forward connected\n";

    asio::signal_set signals(ex, SIGINT, SIGTERM);
    signals.async_wait([&](const std::error_code &, int) {
        client_conn.cancel_and_close();
        server_conn.cancel_and_close();
    });

    std::atomic<std::size_t> recorded_count{0};
    auto a = asio::co_spawn(
        ex,
        pump(client_conn,
             server_conn,
             *recorder,
             true,
             opt.session_id_filter,
             opt,
             start,
             recorded_count),
        asio::use_awaitable);
    auto b = asio::co_spawn(
        ex,
        pump(server_conn,
             client_conn,
             *recorder,
             false,
             opt.session_id_filter,
             opt,
             start,
             recorded_count),
        asio::use_awaitable);

    co_await std::move(a);
    co_await std::move(b);

    recorder->flush();
    const auto flush_ec = recorder->last_error();
    if (flush_ec) {
        std::cout << "[recorder] flush failed: " << flush_ec.message() << "\n";
    }
    recorder.reset(); // 关闭文件，便于后处理

    if (opt.include_peer_local) {
        const auto ec = rewrite_jsonl_add_peer_local_fields(
            opt.output, tx_peer, tx_local, rx_peer, rx_local);
        if (ec) {
            std::cout << "[recorder] append peer/local failed: " << ec.message()
                      << "\n";
        }
    }
    co_return 0;
}

} // namespace

int main(int argc, char **argv) {
    const auto opt = parse_args(argc, argv);
    if (!opt.has_value()) {
        print_usage(argv[0]);
        return 2;
    }

    asio::io_context ioc(1);
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
