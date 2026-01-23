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

#include <charconv>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace {

struct Options final {
    std::string listen;
    std::string forward;
    std::string output;
    std::optional<std::uint16_t> session_id_filter{};
    bool include_sml_header{false};
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

        std::cerr << "未知参数: " << a << "\n";
        return std::nullopt;
    }

    if (opt.listen.empty() || opt.forward.empty() || opt.output.empty()) {
        return std::nullopt;
    }
    return opt;
}

static asio::awaitable<void> pump(secs::hsms::Connection &in,
                                  secs::hsms::Connection &out,
                                  secs::tools::MessageRecorder &recorder,
                                  bool is_tx,
                                  std::optional<std::uint16_t> session_id_filter) {
    for (;;) {
        auto [rd_ec, msg] =
            co_await in.async_read_message();
        if (rd_ec) {
            out.cancel_and_close();
            co_return;
        }

        if (msg.is_data()) {
            if (!session_id_filter.has_value() ||
                msg.header.session_id == *session_id_filter) {
                secs::protocol::DataMessage dm{};
                dm.stream = msg.stream();
                dm.function = msg.function();
                dm.w_bit = msg.w_bit();
                dm.system_bytes = msg.header.system_bytes;
                dm.body = msg.body; // 录制需要保留一份副本（转发仍要使用 msg）
                if (is_tx) {
                    recorder.record_tx(dm);
                } else {
                    recorder.record_rx(dm);
                }
            }
        }

        const auto wr_ec = co_await out.async_write_message(msg);
        if (wr_ec) {
            in.cancel_and_close();
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

    secs::tools::MessageRecorder recorder(opt.output);
    if (!recorder.is_open()) {
        std::cout << "打开输出文件失败: " << opt.output << " ec="
                  << recorder.last_error().message() << "\n";
        co_return 2;
    }
    recorder.set_include_sml_header(opt.include_sml_header);

    asio::ip::tcp::acceptor acceptor(ex, listen_ep);
    std::cout << "[recorder] listen: " << listen_ep << "\n";
    std::cout << "[recorder] forward: " << forward_ep << "\n";
    std::cout << "[recorder] output: " << opt.output << "\n";
    if (opt.session_id_filter.has_value()) {
        std::cout << "[recorder] session-id filter: 0x" << std::hex
                  << *opt.session_id_filter << std::dec << "\n";
    }

    auto [acc_ec, client_socket] =
        co_await acceptor.async_accept(asio::as_tuple(asio::use_awaitable));
    if (acc_ec) {
        std::cout << "[recorder] accept failed: " << acc_ec.message() << "\n";
        co_return 1;
    }
    std::cout << "[recorder] client connected\n";

    secs::hsms::Connection client_conn(std::move(client_socket));
    secs::hsms::Connection server_conn(ex);
    const auto conn_ec = co_await server_conn.async_connect(forward_ep);
    if (conn_ec) {
        std::cout << "[recorder] connect forward failed: " << conn_ec.message()
                  << "\n";
        co_return 1;
    }
    std::cout << "[recorder] forward connected\n";

    asio::signal_set signals(ex, SIGINT, SIGTERM);
    signals.async_wait([&](const std::error_code &, int) {
        client_conn.cancel_and_close();
        server_conn.cancel_and_close();
    });

    auto a = asio::co_spawn(
        ex,
        pump(client_conn,
             server_conn,
             recorder,
             true,
             opt.session_id_filter),
        asio::use_awaitable);
    auto b = asio::co_spawn(
        ex,
        pump(server_conn,
             client_conn,
             recorder,
             false,
             opt.session_id_filter),
        asio::use_awaitable);

    co_await std::move(a);
    co_await std::move(b);

    recorder.flush();
    if (recorder.last_error()) {
        std::cout << "[recorder] flush failed: "
                  << recorder.last_error().message() << "\n";
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
