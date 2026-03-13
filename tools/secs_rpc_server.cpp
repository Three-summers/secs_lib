#include "secs/rpc/server.hpp"

#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <thread>

namespace {

struct Options final {
    std::string listen_address{"127.0.0.1:50051"};
    int idle_timeout_sec{-1};
    int num_threads{0};
    int internal_port{-1};
    bool enable_builtin_services{true};
    std::string enabled_protocols{};
};

std::atomic_bool g_stop_requested{false};

void on_signal(int) noexcept {
    // 只做最小状态标记，真正的 stop/join 留在主线程执行。
    g_stop_requested.store(true, std::memory_order_relaxed);
}

void print_usage(const char *argv0) {
    std::cout << "用法:\n"
              << "  " << argv0 << " [options]\n\n"
              << "选项:\n"
              << "  --listen <ip:port>          监听地址（默认 127.0.0.1:50051）\n"
              << "  --idle-timeout-sec <int>    连接空闲超时秒数（默认 -1）\n"
              << "  --num-threads <int>         brpc 工作线程数（默认 0）\n"
              << "  --internal-port <int>       brpc 内建服务端口（默认 -1）\n"
              << "  --enabled-protocols <text>  协议白名单，空字符串表示全部协议（默认 <all>）\n"
              << "  --disable-builtin-services  关闭 brpc 内建服务\n"
              << "  -h, --help                  显示帮助\n";
}

bool parse_int(std::string_view text, int &out) {
    int value = 0;
    const auto *begin = text.data();
    const auto *end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc{} || ptr != end) {
        return false;
    }
    out = value;
    return true;
}

int parse_args(int argc, char **argv, Options &out) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};

        if (arg == "-h" || arg == "--help") {
            return 1;
        }
        if (arg == "--disable-builtin-services") {
            out.enable_builtin_services = false;
            continue;
        }
        if (arg == "--listen") {
            if (i + 1 >= argc) {
                std::cerr << "missing value for --listen\n";
                return -1;
            }
            out.listen_address = argv[++i];
            continue;
        }
        if (arg == "--enabled-protocols") {
            if (i + 1 >= argc) {
                std::cerr << "missing value for --enabled-protocols\n";
                return -1;
            }
            out.enabled_protocols = argv[++i];
            continue;
        }
        if (arg == "--idle-timeout-sec") {
            if (i + 1 >= argc) {
                std::cerr << "missing value for --idle-timeout-sec\n";
                return -1;
            }
            if (!parse_int(argv[++i], out.idle_timeout_sec)) {
                std::cerr << "invalid value for --idle-timeout-sec\n";
                return -1;
            }
            continue;
        }
        if (arg == "--num-threads") {
            if (i + 1 >= argc) {
                std::cerr << "missing value for --num-threads\n";
                return -1;
            }
            if (!parse_int(argv[++i], out.num_threads)) {
                std::cerr << "invalid value for --num-threads\n";
                return -1;
            }
            continue;
        }
        if (arg == "--internal-port") {
            if (i + 1 >= argc) {
                std::cerr << "missing value for --internal-port\n";
                return -1;
            }
            if (!parse_int(argv[++i], out.internal_port)) {
                std::cerr << "invalid value for --internal-port\n";
                return -1;
            }
            continue;
        }

        std::cerr << "unknown option: " << arg << "\n";
        return -1;
    }
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    Options options;
    const int parse_rc = parse_args(argc, argv, options);
    if (parse_rc > 0) {
        print_usage(argv[0]);
        return 0;
    }
    if (parse_rc < 0) {
        print_usage(argv[0]);
        return 1;
    }

    std::signal(SIGINT, on_signal);
#ifdef SIGTERM
    std::signal(SIGTERM, on_signal);
#endif

    secs::rpc::Server server;
    secs::rpc::ServerOptions server_options;
    server_options.listen_address = options.listen_address;
    server_options.idle_timeout_sec = options.idle_timeout_sec;
    server_options.num_threads = options.num_threads;
    server_options.internal_port = options.internal_port;
    server_options.enable_builtin_services = options.enable_builtin_services;
    server_options.enabled_protocols = options.enabled_protocols;

    const std::error_code ec = server.start(server_options);
    if (ec) {
        std::cerr << "failed to start secs-rpc-server on "
                  << server_options.listen_address << ": ["
                  << ec.category().name() << "] " << ec.message() << "\n";
        return 1;
    }

    std::cout << "secs-rpc-server listening on " << server_options.listen_address
              << ", enabled_protocols="
              << (server_options.enabled_protocols.empty()
                      ? std::string{"<all>"}
                      : server_options.enabled_protocols)
              << "\n";
    std::cout.flush();

    while (!g_stop_requested.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }

    server.stop();
    server.join();
    return 0;
}
