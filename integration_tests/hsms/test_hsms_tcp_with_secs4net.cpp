#include "secs/core/common.hpp"
#include "secs/core/error.hpp"
#
#include "secs/hsms/message.hpp"
#include "secs/hsms/session.hpp"
#
#include "secs/ii/codec.hpp"
#include "secs/ii/item.hpp"
#
#include <asio/as_tuple.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#
#include <arpa/inet.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#
namespace {
#
using secs::core::byte;
using secs::core::bytes_view;
#
using namespace std::chrono_literals;
#
#ifndef SECS_HSMS_DOTNET_EXECUTABLE
#define SECS_HSMS_DOTNET_EXECUTABLE "dotnet"
#endif
#
#ifndef SECS_HSMS_PEER_DLL
#define SECS_HSMS_PEER_DLL ""
#endif
#
struct ChildProcess final {
    pid_t pid{-1};
};
#
static std::error_code make_errno_ec() noexcept {
    return std::error_code(errno, std::generic_category());
}
#
static bool is_socket_not_permitted(const std::error_code &ec) noexcept {
    return ec.category() == std::generic_category() &&
           (ec.value() == EPERM || ec.value() == EACCES);
}
#
static std::pair<std::error_code, std::uint16_t>
allocate_free_port() noexcept {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return {make_errno_ec(), 0};
    }
#
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(0);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
#
    if (::bind(fd, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) != 0) {
        const auto ec = make_errno_ec();
        ::close(fd);
        return {ec, 0};
    }
#
    socklen_t len = sizeof(addr);
    if (::getsockname(fd, reinterpret_cast<sockaddr *>(&addr), &len) != 0) {
        const auto ec = make_errno_ec();
        ::close(fd);
        return {ec, 0};
    }
#
    ::close(fd);
    return {std::error_code{}, ntohs(addr.sin_port)};
}
#
static std::pair<std::error_code, ChildProcess>
spawn_dotnet_peer_tcp(std::string_view mode,
                      std::string_view ip,
                      std::uint16_t port,
                      std::uint16_t device_id) noexcept {
    const pid_t pid = ::fork();
    if (pid < 0) {
        return {make_errno_ec(), ChildProcess{}};
    }
#
    if (pid == 0) {
        (void)::setenv("DOTNET_SKIP_FIRST_TIME_EXPERIENCE", "1", 1);
        (void)::setenv("DOTNET_CLI_TELEMETRY_OPTOUT", "1", 1);
        (void)::setenv("DOTNET_NOLOGO", "1", 1);
#
        const std::string mode_str(mode);
        const std::string ip_str(ip);
        const std::string port_str = std::to_string(port);
        const std::string device_id_str = std::to_string(device_id);
#
        std::vector<std::string> argv_s{
            std::string(SECS_HSMS_DOTNET_EXECUTABLE),
            std::string(SECS_HSMS_PEER_DLL),
            "--mode",
            mode_str,
            "--ip",
            ip_str,
            "--port",
            port_str,
            "--device-id",
            device_id_str,
        };
#
        std::vector<char *> argv;
        argv.reserve(argv_s.size() + 1);
        for (auto &s : argv_s) {
            argv.push_back(s.data());
        }
        argv.push_back(nullptr);
#
        ::execvp(argv[0], argv.data());
        std::cerr << "execvp(dotnet) 失败: " << std::strerror(errno) << "\n";
        std::_Exit(127);
    }
#
    return {std::error_code{}, ChildProcess{.pid = pid}};
}
#
static asio::awaitable<std::pair<std::error_code, int>>
async_wait_exit(pid_t pid, secs::core::duration timeout) {
    auto ex = co_await asio::this_coro::executor;
    asio::steady_timer timer(ex);
    const auto deadline = secs::core::steady_clock::now() + timeout;
#
    for (;;) {
        int status = 0;
        const auto r = ::waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            if (WIFEXITED(status)) {
                co_return std::pair{std::error_code{}, WEXITSTATUS(status)};
            }
            if (WIFSIGNALED(status)) {
                co_return std::pair{std::error_code{}, 128 + WTERMSIG(status)};
            }
            co_return std::pair{
                secs::core::make_error_code(secs::core::errc::invalid_argument),
                -1};
        }
        if (r < 0) {
            co_return std::pair{make_errno_ec(), -1};
        }
#
        const auto now = secs::core::steady_clock::now();
        if (now >= deadline) {
            co_return std::pair{
                secs::core::make_error_code(secs::core::errc::timeout), -1};
        }
#
        timer.expires_after(10ms);
        (void)co_await timer.async_wait(asio::use_awaitable);
    }
}
#
static secs::ii::Item make_test_item() {
    return secs::ii::Item::list({
        secs::ii::Item::u4({123U}),
        secs::ii::Item::ascii("HELLO"),
        secs::ii::Item::list({
            secs::ii::Item::u1({1U, 2U, 3U}),
        }),
    });
}
#
static asio::awaitable<int> run_case_cpp_active_dotnet_passive() {
    auto ex = co_await asio::this_coro::executor;
    constexpr std::uint16_t device_id = 1;
    constexpr std::string_view ip = "127.0.0.1";
#
    auto [port_ec, port] = allocate_free_port();
    if (port_ec) {
        if (is_socket_not_permitted(port_ec)) {
            std::cerr << "[HSMS][tcp][case1] socket/bind 被禁用，跳过\n";
            co_return 77;
        }
        std::cerr << "[HSMS][tcp][case1] 分配端口失败: " << port_ec.message()
                  << "\n";
        co_return 2;
    }
#
    auto [spawn_ec, proc] =
        spawn_dotnet_peer_tcp("tcp-echo-server", ip, port, device_id);
    if (spawn_ec) {
        std::cerr << "[HSMS][tcp][case1] 启动 dotnet 失败: " << spawn_ec.message()
                  << "\n";
        co_return 3;
    }
#
    secs::hsms::SessionOptions options{};
    options.session_id = device_id;
    options.t3 = 5s;
    options.t6 = 5s;
    options.t7 = 5s;
    options.t8 = 2s;
    options.auto_reconnect = false;
#
    const asio::ip::tcp::endpoint endpoint(asio::ip::make_address(std::string(ip)),
                                           port);
#
    std::optional<secs::hsms::Session> session_opt;
    std::error_code open_ec{};
    for (int i = 0; i < 50; ++i) {
        session_opt.emplace(ex, options);
        open_ec = co_await session_opt->async_open_active(endpoint);
        if (!open_ec) {
            break;
        }
        session_opt->stop();
        session_opt.reset();
        asio::steady_timer timer(ex);
        timer.expires_after(50ms);
        (void)co_await timer.async_wait(asio::use_awaitable);
    }
    if (open_ec) {
        std::cerr << "[HSMS][tcp][case1] C++ active SELECT 失败: "
                  << open_ec.message() << "\n";
        ::kill(proc.pid, SIGKILL);
        (void)co_await async_wait_exit(proc.pid, 3s);
        co_return 4;
    }
#
    auto &session = *session_opt;
    if (auto ec = co_await session.async_linktest(); ec) {
        std::cerr << "[HSMS][tcp][case1] LINKTEST 失败: " << ec.message() << "\n";
        session.stop();
        ::kill(proc.pid, SIGKILL);
        (void)co_await async_wait_exit(proc.pid, 3s);
        co_return 5;
    }
#
    const auto item = make_test_item();
    std::vector<byte> body;
    if (auto enc_ec = secs::ii::encode(item, body); enc_ec) {
        std::cerr << "[HSMS][tcp][case1] SECS-II 编码失败: " << enc_ec.message()
                  << "\n";
        session.stop();
        ::kill(proc.pid, SIGKILL);
        (void)co_await async_wait_exit(proc.pid, 3s);
        co_return 6;
    }
#
    auto [req_ec, rsp] = co_await session.async_request_data(
        1, 13, bytes_view{body.data(), body.size()}, 5s);
    if (req_ec) {
        std::cerr << "[HSMS][tcp][case1] 请求-响应失败: " << req_ec.message()
                  << "\n";
        session.stop();
        ::kill(proc.pid, SIGKILL);
        (void)co_await async_wait_exit(proc.pid, 3s);
        co_return 7;
    }
#
    if (rsp.stream() != 1 || rsp.function() != 14 || rsp.w_bit()) {
        std::cerr << "[HSMS][tcp][case1] 响应头不符合预期: stream="
                  << static_cast<int>(rsp.stream())
                  << " function=" << static_cast<int>(rsp.function())
                  << " w=" << (rsp.w_bit() ? 1 : 0) << "\n";
        session.stop();
        ::kill(proc.pid, SIGKILL);
        (void)co_await async_wait_exit(proc.pid, 3s);
        co_return 8;
    }
#
    secs::ii::Item decoded = secs::ii::Item::binary({});
    std::size_t consumed = 0;
    if (auto dec_ec = secs::ii::decode_one(
            bytes_view{rsp.body.data(), rsp.body.size()}, decoded, consumed);
        dec_ec) {
        std::cerr << "[HSMS][tcp][case1] 响应 body 解码失败: " << dec_ec.message()
                  << "\n";
        session.stop();
        ::kill(proc.pid, SIGKILL);
        (void)co_await async_wait_exit(proc.pid, 3s);
        co_return 9;
    }
    if (consumed != rsp.body.size()) {
        std::cerr << "[HSMS][tcp][case1] 响应 body 未完全消费\n";
        session.stop();
        ::kill(proc.pid, SIGKILL);
        (void)co_await async_wait_exit(proc.pid, 3s);
        co_return 10;
    }
    if (decoded != item) {
        std::cerr << "[HSMS][tcp][case1] 响应 Item 与请求不一致\n";
        session.stop();
        ::kill(proc.pid, SIGKILL);
        (void)co_await async_wait_exit(proc.pid, 3s);
        co_return 11;
    }
#
    session.stop();
#
    auto [wait_ec, exit_code] = co_await async_wait_exit(proc.pid, 10s);
    if (wait_ec) {
        std::cerr << "[HSMS][tcp][case1] 等待 dotnet 退出失败: " << wait_ec.message()
                  << "\n";
        ::kill(proc.pid, SIGKILL);
        (void)co_await async_wait_exit(proc.pid, 3s);
        co_return 12;
    }
    if (exit_code != 0) {
        std::cerr << "[HSMS][tcp][case1] dotnet 返回码异常: " << exit_code << "\n";
        co_return 13;
    }
#
    co_return 0;
}
#
static asio::awaitable<int> run_case_dotnet_active_cpp_passive() {
    auto ex = co_await asio::this_coro::executor;
    constexpr std::uint16_t device_id = 1;
    constexpr std::string_view ip = "127.0.0.1";
#
    auto [port_ec, port] = allocate_free_port();
    if (port_ec) {
        if (is_socket_not_permitted(port_ec)) {
            std::cerr << "[HSMS][tcp][case2] socket/bind 被禁用，跳过\n";
            co_return 77;
        }
        std::cerr << "[HSMS][tcp][case2] 分配端口失败: " << port_ec.message()
                  << "\n";
        co_return 20;
    }
#
    asio::ip::tcp::acceptor acceptor(
        ex, asio::ip::tcp::endpoint(asio::ip::make_address(std::string(ip)), port));
#
    auto [spawn_ec, proc] =
        spawn_dotnet_peer_tcp("tcp-request-client", ip, port, device_id);
    if (spawn_ec) {
        std::cerr << "[HSMS][tcp][case2] 启动 dotnet 失败: " << spawn_ec.message()
                  << "\n";
        co_return 21;
    }
#
    auto [acc_ec, socket] =
        co_await acceptor.async_accept(asio::as_tuple(asio::use_awaitable));
    if (acc_ec) {
        std::cerr << "[HSMS][tcp][case2] accept 失败: " << acc_ec.message()
                  << "\n";
        ::kill(proc.pid, SIGKILL);
        (void)co_await async_wait_exit(proc.pid, 3s);
        co_return 22;
    }
#
    secs::hsms::SessionOptions options{};
    options.session_id = device_id;
    options.t3 = 5s;
    options.t6 = 5s;
    options.t7 = 5s;
    options.t8 = 2s;
    options.auto_reconnect = false;
#
    secs::hsms::Session session(ex, options);
    if (auto ec = co_await session.async_open_passive(std::move(socket)); ec) {
        std::cerr << "[HSMS][tcp][case2] C++ passive SELECT 失败: " << ec.message()
                  << "\n";
        session.stop();
        ::kill(proc.pid, SIGKILL);
        (void)co_await async_wait_exit(proc.pid, 3s);
        co_return 23;
    }
#
    auto [rec_ec, req] = co_await session.async_receive_data(10s);
    if (rec_ec) {
        std::cerr << "[HSMS][tcp][case2] 接收 data message 失败: " << rec_ec.message()
                  << "\n";
        session.stop();
        ::kill(proc.pid, SIGKILL);
        (void)co_await async_wait_exit(proc.pid, 3s);
        co_return 24;
    }

    std::cerr << "[HSMS][tcp][case2] recv: session_id=0x"
              << std::hex << req.header.session_id << std::dec
              << " S=" << static_cast<int>(req.stream())
              << " F=" << static_cast<int>(req.function())
              << " W=" << (req.w_bit() ? 1 : 0)
              << " SB=0x" << std::hex << req.header.system_bytes << std::dec
              << " body=" << req.body.size() << "B\n";
#
    secs::ii::Item decoded = secs::ii::Item::binary({});
    std::size_t consumed = 0;
    if (auto dec_ec = secs::ii::decode_one(
            bytes_view{req.body.data(), req.body.size()}, decoded, consumed);
        dec_ec) {
        std::cerr << "[HSMS][tcp][case2] 请求 body 解码失败: " << dec_ec.message()
                  << "\n";
        session.stop();
        ::kill(proc.pid, SIGKILL);
        (void)co_await async_wait_exit(proc.pid, 3s);
        co_return 25;
    }
    if (consumed != req.body.size()) {
        std::cerr << "[HSMS][tcp][case2] 请求 body 未完全消费\n";
        session.stop();
        ::kill(proc.pid, SIGKILL);
        (void)co_await async_wait_exit(proc.pid, 3s);
        co_return 26;
    }
#
    const auto expected = make_test_item();
    if (decoded != expected) {
        std::cerr << "[HSMS][tcp][case2] 入站 Item 与预期不一致\n";
        session.stop();
        ::kill(proc.pid, SIGKILL);
        (void)co_await async_wait_exit(proc.pid, 3s);
        co_return 27;
    }
#
    std::vector<byte> encoded;
    if (auto enc_ec = secs::ii::encode(decoded, encoded); enc_ec) {
        std::cerr << "[HSMS][tcp][case2] 回包 SECS-II 编码失败: " << enc_ec.message()
                  << "\n";
        session.stop();
        ::kill(proc.pid, SIGKILL);
        (void)co_await async_wait_exit(proc.pid, 3s);
        co_return 28;
    }
#
    const auto rsp = secs::hsms::make_data_message(
        req.header.session_id,
        req.stream(),
        static_cast<std::uint8_t>(req.function() + 1U),
        false,
        req.header.system_bytes,
        bytes_view{encoded.data(), encoded.size()});

    std::cerr << "[HSMS][tcp][case2] send: session_id=0x"
              << std::hex << rsp.header.session_id << std::dec
              << " S=" << static_cast<int>(rsp.stream())
              << " F=" << static_cast<int>(rsp.function())
              << " W=" << (rsp.w_bit() ? 1 : 0)
              << " SB=0x" << std::hex << rsp.header.system_bytes << std::dec
              << " body=" << rsp.body.size() << "B\n";
#
    if (auto send_ec = co_await session.async_send(rsp); send_ec) {
        std::cerr << "[HSMS][tcp][case2] 发送回包失败: " << send_ec.message()
                  << "\n";
        session.stop();
        ::kill(proc.pid, SIGKILL);
        (void)co_await async_wait_exit(proc.pid, 3s);
        co_return 29;
    }
#
    // 不要在发送后立刻 close：避免对端在处理回包前因连接关闭触发重连/清空缓冲。
    auto [wait_ec, exit_code] = co_await async_wait_exit(proc.pid, 10s);
    if (wait_ec) {
        std::cerr << "[HSMS][tcp][case2] 等待 dotnet 退出失败: " << wait_ec.message()
                  << "\n";
        session.stop();
        ::kill(proc.pid, SIGKILL);
        (void)co_await async_wait_exit(proc.pid, 3s);
        co_return 30;
    }
    if (exit_code != 0) {
        std::cerr << "[HSMS][tcp][case2] dotnet 返回码异常: " << exit_code << "\n";
        session.stop();
        co_return 31;
    }
#
    session.stop();
#
    co_return 0;
}
#
asio::awaitable<int> run_all() {
    const int rc1 = co_await run_case_cpp_active_dotnet_passive();
    if (rc1 != 0) {
        co_return rc1;
    }
    const int rc2 = co_await run_case_dotnet_active_cpp_passive();
    if (rc2 != 0) {
        co_return rc2;
    }
    co_return 0;
}
#
} // namespace
#
int main() {
    ::signal(SIGPIPE, SIG_IGN);
#
    asio::io_context io;
    int rc = 1;
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            rc = co_await run_all();
            io.stop();
        },
        asio::detached);
    io.run();
    return rc;
}
