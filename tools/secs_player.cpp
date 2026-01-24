#include "secs/hsms/message.hpp"
#include "secs/hsms/session.hpp"
#include "secs/tools/recording.hpp"
#include "secs/utils/hex.hpp"

#include <asio/as_tuple.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/signal_set.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

#include <charconv>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace {

struct Options final {
    std::string input;
    std::string connect;
    std::uint16_t session_id{0x0001};

    secs::tools::PlaybackMode mode{secs::tools::PlaybackMode::fast};
    double speed{1.0};

    secs::core::duration rx_timeout{std::chrono::seconds{5}};

    enum class DirFilter : std::uint8_t { any = 0, tx = 1, rx = 2 };
    std::vector<std::pair<std::uint8_t, std::uint8_t>> only_sf{};
    std::vector<std::uint8_t> only_streams{};
    DirFilter dir{DirFilter::any};
    std::optional<std::uint64_t> since_us{};
    std::optional<std::uint64_t> until_us{};
    std::optional<std::size_t> limit{};

    bool verbose{false};
    bool stats{false};
    bool ignore_system_bytes{false};
    bool continue_on_mismatch{false};
};

static void print_usage(const char *argv0) {
    std::cout << "用法:\n"
              << "  " << argv0
              << " --input <capture.jsonl> --connect <ip:port> [options]\n\n"
              << "说明:\n"
              << "  - 连接 HSMS 目标并按录制文件回放 data message（JSONL）\n"
              << "  - 遇到 dir=TX：发送；dir=RX：等待并校验下一条入站 data message\n\n"
              << "选项:\n"
              << "  --input <path>          录制文件（JSONL）\n"
              << "  --connect <ip:port>     连接地址（例如 127.0.0.1:5000）\n"
              << "  --session-id <u16>      HSMS data SessionID（支持 0x 前缀，默认 0x0001）\n"
              << "  --mode <fast|realtime>  回放模式（默认 fast）\n"
              << "  --speed <double>        realtime 速度倍率（默认 1.0）\n"
              << "  --timeout-ms <u32>      等待 RX 的超时毫秒（默认 5000）\n"
              << "  --only s=<u8>,f=<u8>    仅回放指定 S/F（可重复指定）\n"
              << "  --only-stream <u8>      仅回放指定 Stream（可重复指定）\n"
              << "  --dir <tx|rx>           仅回放指定方向（默认不过滤）\n"
              << "  --since-us <u64>        仅回放 ts_us >= since-us 的消息\n"
              << "  --until-us <u64>        仅回放 ts_us <= until-us 的消息\n"
              << "  --limit <N>             最多回放 N 条（按过滤后的消息计数）\n"
              << "  --ignore-system-bytes   校验时忽略 system_bytes\n"
              << "  --continue-on-mismatch  校验失败继续回放（默认失败即退出）\n"
              << "  --verbose               输出更详细的差异（包含 body hexdump）\n"
              << "  --stats                 结束时输出统计信息\n"
              << "  -h, --help              显示帮助\n";
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

static bool parse_mode(std::string_view s, secs::tools::PlaybackMode &out) {
    if (s == "fast") {
        out = secs::tools::PlaybackMode::fast;
        return true;
    }
    if (s == "realtime") {
        out = secs::tools::PlaybackMode::realtime;
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
        if (a == "--input") {
            const char *v = need_value("--input");
            if (!v) {
                return std::nullopt;
            }
            opt.input = v;
            continue;
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
        if (a == "--mode") {
            const char *v = need_value("--mode");
            if (!v || !parse_mode(v, opt.mode)) {
                std::cerr << "非法 mode: " << (v ? v : "")
                          << "（期望 fast|realtime）\n";
                return std::nullopt;
            }
            continue;
        }
        if (a == "--speed") {
            const char *v = need_value("--speed");
            if (!v) {
                return std::nullopt;
            }
            try {
                opt.speed = std::stod(std::string(v));
            } catch (...) {
                std::cerr << "非法 speed: " << v << "\n";
                return std::nullopt;
            }
            continue;
        }
        if (a == "--timeout-ms") {
            const char *v = need_value("--timeout-ms");
            if (!v) {
                return std::nullopt;
            }
            std::uint32_t ms = 0;
            if (!parse_u32(v, ms)) {
                std::cerr << "非法 timeout-ms: " << v << "\n";
                return std::nullopt;
            }
            opt.rx_timeout = std::chrono::milliseconds{ms};
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
        if (a == "--ignore-system-bytes") {
            opt.ignore_system_bytes = true;
            continue;
        }
        if (a == "--continue-on-mismatch") {
            opt.continue_on_mismatch = true;
            continue;
        }
        if (a == "--verbose") {
            opt.verbose = true;
            continue;
        }
        if (a == "--stats") {
            opt.stats = true;
            continue;
        }

        std::cerr << "未知参数: " << a << "\n";
        return std::nullopt;
    }

    if (opt.input.empty() || opt.connect.empty()) {
        return std::nullopt;
    }
    if (opt.since_us.has_value() && opt.until_us.has_value() &&
        *opt.since_us > *opt.until_us) {
        std::cerr << "非法时间窗：since-us > until-us\n";
        return std::nullopt;
    }
    return opt;
}

static void print_mismatch(const secs::tools::RecordedMessage &expected,
                           const secs::hsms::Message &actual,
                           bool ignore_system_bytes,
                           bool verbose) {
    std::cout << "✗ mismatch\n";
    std::cout << "  expected: dir=RX S" << static_cast<int>(expected.stream)
              << "F" << static_cast<int>(expected.function) << " W="
              << (expected.w_bit ? 1 : 0) << " sb=" << expected.system_bytes
              << " body_n=" << expected.body.size() << "\n";
    std::cout << "  actual:   dir=RX S" << static_cast<int>(actual.stream())
              << "F" << static_cast<int>(actual.function())
              << " W=" << (actual.w_bit() ? 1 : 0)
              << " sb=" << actual.header.system_bytes
              << " body_n=" << actual.body.size() << "\n";

    if (!ignore_system_bytes && expected.system_bytes != actual.header.system_bytes) {
        std::cout << "  - system_bytes mismatch\n";
    }
    if (expected.stream != actual.stream() || expected.function != actual.function() ||
        expected.w_bit != actual.w_bit()) {
        std::cout << "  - header mismatch\n";
    }
    if (expected.body != actual.body) {
        std::cout << "  - body mismatch\n";
        if (verbose) {
            std::cout << "  expected body:\n"
                      << secs::utils::hex_dump(
                             secs::core::bytes_view{expected.body.data(),
                                                    expected.body.size()})
                      << "  actual body:\n"
                      << secs::utils::hex_dump(
                             secs::core::bytes_view{actual.body.data(),
                                                    actual.body.size()});
        }
    }
}

static asio::awaitable<int> run(const Options &opt) {
    auto ex = co_await asio::this_coro::executor;

    asio::ip::tcp::endpoint ep;
    if (!parse_endpoint(opt.connect, ep)) {
        std::cout << "非法 connect: " << opt.connect << "\n";
        co_return 2;
    }

    secs::tools::MessagePlayer player(opt.input);
    player.set_mode(opt.mode);
    player.set_speed(opt.speed);
    if (!player.is_open()) {
        std::cout << "打开录制文件失败: " << opt.input << " ec="
                  << player.last_error().message() << "\n";
        co_return 2;
    }

    secs::hsms::SessionOptions hsms_opt{};
    hsms_opt.session_id = opt.session_id;
    hsms_opt.t3 = 45s;
    hsms_opt.t5 = 1s;
    hsms_opt.t6 = 5s;
    hsms_opt.t7 = 10s;
    hsms_opt.t8 = 5s;
    hsms_opt.linktest_interval = 0s;
    hsms_opt.auto_reconnect = false;
    hsms_opt.passive_accept_select = true;

    secs::hsms::Session sess(ex, hsms_opt);
    const auto open_ec = co_await sess.async_open_active(ep);
    if (open_ec) {
        std::cout << "HSMS open failed: " << open_ec.message() << "\n";
        co_return 1;
    }
    std::cout << "[player] selected: " << ep << " session_id=0x" << std::hex
              << opt.session_id << std::dec << "\n";

    asio::signal_set signals(ex, SIGINT, SIGTERM);
    signals.async_wait([&](const std::error_code &, int) { sess.stop(); });

    std::size_t total = 0;
    std::size_t tx = 0;
    std::size_t rx = 0;
    std::size_t skipped = 0;
    std::size_t mismatches = 0;

    const auto matches_selector = [&](const secs::tools::RecordedMessage &m) -> bool {
        const bool has_selector =
            !opt.only_sf.empty() || !opt.only_streams.empty();
        if (has_selector) {
            bool ok = false;
            for (const auto s : opt.only_streams) {
                if (m.stream == s) {
                    ok = true;
                    break;
                }
            }
            if (!ok) {
                for (const auto &sf : opt.only_sf) {
                    if (m.stream == sf.first && m.function == sf.second) {
                        ok = true;
                        break;
                    }
                }
            }
            if (!ok) {
                return false;
            }
        }

        if (opt.dir == Options::DirFilter::tx && !m.is_tx) {
            return false;
        }
        if (opt.dir == Options::DirFilter::rx && m.is_tx) {
            return false;
        }

        if (opt.since_us.has_value() && m.timestamp_us < *opt.since_us) {
            return false;
        }
        if (opt.until_us.has_value() && m.timestamp_us > *opt.until_us) {
            return false;
        }
        return true;
    };

    for (;;) {
        auto m = player.next_message();
        if (!m.has_value()) {
            if (player.last_error()) {
                std::cout << "解析录制文件失败: " << player.last_error().message()
                          << "\n";
                sess.stop();
                (void)co_await sess.async_wait_reader_stopped(1s);
                co_return 2;
            }
            break;
        }
        if (opt.until_us.has_value() && m->timestamp_us > *opt.until_us) {
            break;
        }
        if (!matches_selector(*m)) {
            ++skipped;
            continue;
        }
        if (opt.limit.has_value() && total >= *opt.limit) {
            break;
        }
        ++total;

        if (m->is_tx) {
            secs::hsms::Message wire = secs::hsms::make_data_message(
                opt.session_id,
                m->stream,
                m->function,
                m->w_bit,
                m->system_bytes,
                secs::core::bytes_view{m->body.data(), m->body.size()});
            const auto ec = co_await sess.async_send(wire);
            if (ec) {
                std::cout << "TX send failed: " << ec.message() << "\n";
                sess.stop();
                (void)co_await sess.async_wait_reader_stopped(1s);
                co_return 1;
            }
            ++tx;
            continue;
        }

        auto [ec, actual] = co_await sess.async_receive_data(opt.rx_timeout);
        if (ec) {
            std::cout << "RX receive failed: " << ec.message() << "\n";
            sess.stop();
            (void)co_await sess.async_wait_reader_stopped(1s);
            co_return 1;
        }
        ++rx;

        bool ok = true;
        if (actual.stream() != m->stream || actual.function() != m->function ||
            actual.w_bit() != m->w_bit) {
            ok = false;
        }
        if (!opt.ignore_system_bytes &&
            actual.header.system_bytes != m->system_bytes) {
            ok = false;
        }
        if (actual.body != m->body) {
            ok = false;
        }

        if (!ok) {
            ++mismatches;
            print_mismatch(*m, actual, opt.ignore_system_bytes, opt.verbose);
            if (!opt.continue_on_mismatch) {
                sess.stop();
                (void)co_await sess.async_wait_reader_stopped(1s);
                co_return 1;
            }
        }
    }

    sess.stop();
    (void)co_await sess.async_wait_reader_stopped(1s);

    if (opt.stats) {
        const auto st = player.get_stats();
        std::cout << "\n=== stats ===\n"
                  << "messages_total=" << total << " tx=" << tx << " rx=" << rx
                  << " skipped=" << skipped << " mismatches=" << mismatches
                  << "\n"
                  << "file_total_lines=" << st.total_lines
                  << " parsed_messages=" << st.parsed_messages
                  << " parse_errors=" << st.parse_errors << "\n";
    }

    if (mismatches != 0) {
        co_return 1;
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
