#include "secs/core/common.hpp"
#include "secs/core/error.hpp"
#include "secs/core/event.hpp"
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
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
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
static secs::hsms::SessionOptions make_default_opts(std::uint16_t device_id) {
    secs::hsms::SessionOptions options{};
    options.session_id = device_id;
    options.t3 = 5s;
    options.t6 = 5s;
    options.t7 = 5s;
    options.t8 = 2s;
    options.auto_reconnect = false;
    return options;
}
#
static secs::ii::Item make_small_item(std::uint32_t tag) {
    return secs::ii::Item::list({
        secs::ii::Item::u4({tag}),
        secs::ii::Item::ascii("HELLO"),
        secs::ii::Item::list({
            secs::ii::Item::u1({1U, 2U, 3U}),
        }),
    });
}
#
static secs::ii::Item make_large_binary_item(std::size_t n) {
    std::vector<byte> data;
    data.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        data[i] = static_cast<byte>(i & 0xFFU);
    }
    return secs::ii::Item::binary(std::move(data));
}
#
static std::pair<std::error_code, asio::ip::tcp::endpoint>
make_endpoint(std::string_view ip, std::uint16_t port) noexcept {
    try {
        return {std::error_code{},
                asio::ip::tcp::endpoint(asio::ip::make_address(std::string(ip)),
                                        port)};
    } catch (const std::exception &e) {
        std::cerr << "解析 endpoint 失败: " << e.what() << "\n";
        return {secs::core::make_error_code(secs::core::errc::invalid_argument),
                asio::ip::tcp::endpoint{}};
    }
}
#
static asio::awaitable<
    std::pair<std::error_code, std::unique_ptr<secs::hsms::Session>>>
open_active_with_retries(secs::hsms::SessionOptions options,
                         asio::ip::tcp::endpoint endpoint,
                         int retries,
                         secs::core::duration backoff) {
    auto ex = co_await asio::this_coro::executor;
#
    for (int i = 0; i < retries; ++i) {
        auto session = std::make_unique<secs::hsms::Session>(ex, options);
        const auto ec = co_await session->async_open_active(endpoint);
        if (!ec) {
            co_return std::pair{std::error_code{}, std::move(session)};
        }
        session->stop();
#
        asio::steady_timer timer(ex);
        timer.expires_after(backoff);
        (void)co_await timer.async_wait(asio::use_awaitable);
    }
#
    co_return std::pair{
        secs::core::make_error_code(secs::core::errc::timeout),
        std::unique_ptr<secs::hsms::Session>{}};
}
#
static asio::awaitable<int> case_large_payload() {
    auto ex = co_await asio::this_coro::executor;
    constexpr std::uint16_t device_id = 1;
    constexpr std::string_view ip = "127.0.0.1";
#
    auto [port_ec, port] = allocate_free_port();
    if (port_ec) {
        if (is_socket_not_permitted(port_ec)) {
            co_return 77;
        }
        std::cerr << "[HSMS][ext][large] 分配端口失败: " << port_ec.message()
                  << "\n";
        co_return 2;
    }
#
    auto [spawn_ec, proc] =
        spawn_dotnet_peer_tcp("tcp-echo-server", ip, port, device_id);
    if (spawn_ec) {
        std::cerr << "[HSMS][ext][large] 启动 dotnet 失败: " << spawn_ec.message()
                  << "\n";
        co_return 3;
    }
#
    auto [ep_ec, endpoint] = make_endpoint(ip, port);
    if (ep_ec) {
        ::kill(proc.pid, SIGKILL);
        (void)co_await async_wait_exit(proc.pid, 3s);
        co_return 4;
    }
#
    auto options = make_default_opts(device_id);
    auto [open_ec, session] =
        co_await open_active_with_retries(options, endpoint, 50, 50ms);
    if (open_ec) {
        std::cerr << "[HSMS][ext][large] 打开连接失败\n";
        ::kill(proc.pid, SIGKILL);
        (void)co_await async_wait_exit(proc.pid, 3s);
        co_return 5;
    }
#
    const auto item = make_large_binary_item(256U * 1024U);
    std::vector<byte> body;
    if (auto enc_ec = secs::ii::encode(item, body); enc_ec) {
        std::cerr << "[HSMS][ext][large] SECS-II 编码失败: " << enc_ec.message()
                  << "\n";
        session->stop();
        ::kill(proc.pid, SIGKILL);
        (void)co_await async_wait_exit(proc.pid, 3s);
        co_return 6;
    }
#
    auto [req_ec, rsp] = co_await session->async_request_data(
        1, 13, bytes_view{body.data(), body.size()}, 10s);
    if (req_ec) {
        std::cerr << "[HSMS][ext][large] 请求-响应失败: " << req_ec.message()
                  << "\n";
        // 补充诊断：若 dotnet 侧已提前退出，打印退出码（避免仅看到 EOF 无线索）。
        {
            auto [wait_ec, exit_code] = co_await async_wait_exit(proc.pid, 200ms);
            if (!wait_ec) {
                std::cerr << "[HSMS][ext][large] dotnet 已退出: " << exit_code
                          << "\n";
                proc.pid = -1;
            } else if (wait_ec !=
                       secs::core::make_error_code(secs::core::errc::timeout)) {
                std::cerr << "[HSMS][ext][large] waitpid 失败: "
                          << wait_ec.message() << "\n";
            }
        }
        session->stop();
        if (proc.pid > 0) {
            ::kill(proc.pid, SIGKILL);
            (void)co_await async_wait_exit(proc.pid, 3s);
        }
        co_return 7;
    }
#
    secs::ii::Item decoded = secs::ii::Item::binary({});
    std::size_t consumed = 0;
    if (auto dec_ec = secs::ii::decode_one(
            bytes_view{rsp.body.data(), rsp.body.size()}, decoded, consumed);
        dec_ec) {
        std::cerr << "[HSMS][ext][large] 响应 body 解码失败: " << dec_ec.message()
                  << "\n";
        session->stop();
        ::kill(proc.pid, SIGKILL);
        (void)co_await async_wait_exit(proc.pid, 3s);
        co_return 8;
    }
    if (consumed != rsp.body.size()) {
        std::cerr << "[HSMS][ext][large] 响应 body 未完全消费\n";
        session->stop();
        ::kill(proc.pid, SIGKILL);
        (void)co_await async_wait_exit(proc.pid, 3s);
        co_return 9;
    }
    if (decoded != item) {
        std::cerr << "[HSMS][ext][large] 大包回显不一致\n";
        session->stop();
        ::kill(proc.pid, SIGKILL);
        (void)co_await async_wait_exit(proc.pid, 3s);
        co_return 10;
    }
#
    auto [wait_ec, exit_code] = co_await async_wait_exit(proc.pid, 10s);
    if (wait_ec || exit_code != 0) {
        std::cerr << "[HSMS][ext][large] dotnet 返回码异常";
        if (wait_ec) {
            std::cerr << " wait=" << wait_ec.message();
        }
        std::cerr << " exit=" << exit_code << "\n";
        session->stop();
        ::kill(proc.pid, SIGKILL);
        (void)co_await async_wait_exit(proc.pid, 3s);
        co_return 11;
    }
#
    session->stop();
    co_return 0;
}
#
static asio::awaitable<int> case_out_of_order_responses() {
    auto ex = co_await asio::this_coro::executor;
    constexpr std::uint16_t device_id = 1;
    constexpr std::string_view ip = "127.0.0.1";
#
    auto [port_ec, port] = allocate_free_port();
    if (port_ec) {
        if (is_socket_not_permitted(port_ec)) {
            co_return 77;
        }
        std::cerr << "[HSMS][ext][reorder] 分配端口失败: " << port_ec.message()
                  << "\n";
        co_return 20;
    }
#
    auto [spawn_ec, proc] =
        spawn_dotnet_peer_tcp("tcp-reorder-echo-server", ip, port, device_id);
    if (spawn_ec) {
        std::cerr << "[HSMS][ext][reorder] 启动 dotnet 失败: " << spawn_ec.message()
                  << "\n";
        co_return 21;
    }
#
    auto [ep_ec, endpoint] = make_endpoint(ip, port);
    if (ep_ec) {
        ::kill(proc.pid, SIGKILL);
        (void)co_await async_wait_exit(proc.pid, 3s);
        co_return 22;
    }
#
    auto options = make_default_opts(device_id);
    auto [open_ec, session] =
        co_await open_active_with_retries(options, endpoint, 50, 50ms);
    if (open_ec) {
        std::cerr << "[HSMS][ext][reorder] 打开连接失败\n";
        ::kill(proc.pid, SIGKILL);
        (void)co_await async_wait_exit(proc.pid, 3s);
        co_return 23;
    }
#
    const auto item1 = make_small_item(1);
    const auto item2 = make_small_item(2);
#
    std::vector<byte> body1;
    std::vector<byte> body2;
    if (auto ec = secs::ii::encode(item1, body1); ec) {
        std::cerr << "[HSMS][ext][reorder] item1 编码失败: " << ec.message()
                  << "\n";
        session->stop();
        ::kill(proc.pid, SIGKILL);
        (void)co_await async_wait_exit(proc.pid, 3s);
        co_return 24;
    }
    if (auto ec = secs::ii::encode(item2, body2); ec) {
        std::cerr << "[HSMS][ext][reorder] item2 编码失败: " << ec.message()
                  << "\n";
        session->stop();
        ::kill(proc.pid, SIGKILL);
        (void)co_await async_wait_exit(proc.pid, 3s);
        co_return 25;
    }
#
    std::optional<std::pair<std::error_code, secs::hsms::Message>> r1;
    std::optional<std::pair<std::error_code, secs::hsms::Message>> r2;
    secs::core::Event d1{};
    secs::core::Event d2{};
#
    asio::co_spawn(
        ex,
        [&]() -> asio::awaitable<void> {
            auto [ec, rsp] = co_await session->async_request_data(
                1, 13, bytes_view{body1.data(), body1.size()}, 10s);
            r1 = std::pair{ec, std::move(rsp)};
            d1.set();
        },
        asio::detached);
#
    asio::co_spawn(
        ex,
        [&]() -> asio::awaitable<void> {
            auto [ec, rsp] = co_await session->async_request_data(
                1, 13, bytes_view{body2.data(), body2.size()}, 10s);
            r2 = std::pair{ec, std::move(rsp)};
            d2.set();
        },
        asio::detached);
#
    if (auto ec = co_await d1.async_wait(12s); ec) {
        std::cerr << "[HSMS][ext][reorder] 等待 r1 失败: " << ec.message()
                  << "\n";
        session->stop();
        ::kill(proc.pid, SIGKILL);
        (void)co_await async_wait_exit(proc.pid, 3s);
        co_return 26;
    }
    if (auto ec = co_await d2.async_wait(12s); ec) {
        std::cerr << "[HSMS][ext][reorder] 等待 r2 失败: " << ec.message()
                  << "\n";
        session->stop();
        ::kill(proc.pid, SIGKILL);
        (void)co_await async_wait_exit(proc.pid, 3s);
        co_return 27;
    }
    if (!r1.has_value() || !r2.has_value()) {
        std::cerr << "[HSMS][ext][reorder] 缺少结果\n";
        session->stop();
        ::kill(proc.pid, SIGKILL);
        (void)co_await async_wait_exit(proc.pid, 3s);
        co_return 28;
    }
#
    auto check_rsp = [&](const std::pair<std::error_code, secs::hsms::Message> &r,
                         const secs::ii::Item &expected,
                         const char *label) -> bool {
        const auto &ec = r.first;
        const auto &m = r.second;
        if (ec) {
            std::cerr << "[HSMS][ext][reorder] " << label
                      << " 请求失败: " << ec.message() << "\n";
            return false;
        }
        if (m.stream() != 1 || m.function() != 14 || m.w_bit()) {
            std::cerr << "[HSMS][ext][reorder] " << label
                      << " 响应头不符合预期\n";
            return false;
        }
#
        secs::ii::Item decoded = secs::ii::Item::binary({});
        std::size_t consumed = 0;
        if (auto dec_ec = secs::ii::decode_one(
                bytes_view{m.body.data(), m.body.size()}, decoded, consumed);
            dec_ec) {
            std::cerr << "[HSMS][ext][reorder] " << label
                      << " 响应解码失败: " << dec_ec.message() << "\n";
            return false;
        }
        if (consumed != m.body.size()) {
            std::cerr << "[HSMS][ext][reorder] " << label
                      << " 响应存在尾部\n";
            return false;
        }
        if (decoded != expected) {
            std::cerr << "[HSMS][ext][reorder] " << label
                      << " 响应 Item 不一致\n";
            return false;
        }
        return true;
    };
#
    const bool ok =
        (check_rsp(*r1, item1, "r1") && check_rsp(*r2, item2, "r2")) ||
        (check_rsp(*r1, item2, "r1") && check_rsp(*r2, item1, "r2"));
    if (!ok) {
        session->stop();
        ::kill(proc.pid, SIGKILL);
        (void)co_await async_wait_exit(proc.pid, 3s);
        co_return 29;
    }
#
    auto [wait_ec, exit_code] = co_await async_wait_exit(proc.pid, 10s);
    if (wait_ec || exit_code != 0) {
        std::cerr << "[HSMS][ext][reorder] dotnet 返回码异常";
        if (wait_ec) {
            std::cerr << " wait=" << wait_ec.message();
        }
        std::cerr << " exit=" << exit_code << "\n";
        session->stop();
        ::kill(proc.pid, SIGKILL);
        (void)co_await async_wait_exit(proc.pid, 3s);
        co_return 30;
    }
#
    session->stop();
    co_return 0;
}
#
static asio::awaitable<int> case_device_id_mismatch_s9f1() {
    auto ex = co_await asio::this_coro::executor;
    constexpr std::uint16_t local_device_id = 2;
    constexpr std::uint16_t remote_device_id = 1;
    constexpr std::string_view ip = "127.0.0.1";
#
    auto [port_ec, port] = allocate_free_port();
    if (port_ec) {
        if (is_socket_not_permitted(port_ec)) {
            co_return 77;
        }
        std::cerr << "[HSMS][ext][s9f1] 分配端口失败: " << port_ec.message()
                  << "\n";
        co_return 40;
    }
#
    auto [spawn_ec, proc] =
        spawn_dotnet_peer_tcp("tcp-s9f1-server", ip, port, remote_device_id);
    if (spawn_ec) {
        std::cerr << "[HSMS][ext][s9f1] 启动 dotnet 失败: " << spawn_ec.message()
                  << "\n";
        co_return 41;
    }
#
    auto [ep_ec, endpoint] = make_endpoint(ip, port);
    if (ep_ec) {
        ::kill(proc.pid, SIGKILL);
        (void)co_await async_wait_exit(proc.pid, 3s);
        co_return 42;
    }
#
    auto options = make_default_opts(local_device_id);
    auto [open_ec, session] =
        co_await open_active_with_retries(options, endpoint, 50, 50ms);
    if (open_ec) {
        std::cerr << "[HSMS][ext][s9f1] 打开连接失败\n";
        ::kill(proc.pid, SIGKILL);
        (void)co_await async_wait_exit(proc.pid, 3s);
        co_return 43;
    }
#
    const auto item = make_small_item(123);
    std::vector<byte> body;
    if (auto enc_ec = secs::ii::encode(item, body); enc_ec) {
        std::cerr << "[HSMS][ext][s9f1] 编码失败: " << enc_ec.message() << "\n";
        session->stop();
        ::kill(proc.pid, SIGKILL);
        (void)co_await async_wait_exit(proc.pid, 3s);
        co_return 44;
    }
#
    const auto sb = session->allocate_system_bytes();
    const auto req = secs::hsms::make_data_message(
        local_device_id, 1, 13, true, sb, bytes_view{body.data(), body.size()});
    if (auto send_ec = co_await session->async_send(req); send_ec) {
        std::cerr << "[HSMS][ext][s9f1] 发送失败: " << send_ec.message() << "\n";
        session->stop();
        ::kill(proc.pid, SIGKILL);
        (void)co_await async_wait_exit(proc.pid, 3s);
        co_return 45;
    }
#
    auto [rec_ec, rsp] = co_await session->async_receive_data(10s);
    if (rec_ec) {
        std::cerr << "[HSMS][ext][s9f1] 接收失败: " << rec_ec.message() << "\n";
        session->stop();
        ::kill(proc.pid, SIGKILL);
        (void)co_await async_wait_exit(proc.pid, 3s);
        co_return 46;
    }
#
    if (rsp.stream() != 9 || rsp.function() != 1 || rsp.w_bit()) {
        std::cerr << "[HSMS][ext][s9f1] 未收到 S9F1（或 W-bit 异常）\n";
        session->stop();
        ::kill(proc.pid, SIGKILL);
        (void)co_await async_wait_exit(proc.pid, 3s);
        co_return 47;
    }
#
    secs::ii::Item decoded = secs::ii::Item::binary({});
    std::size_t consumed = 0;
    if (auto dec_ec = secs::ii::decode_one(
            bytes_view{rsp.body.data(), rsp.body.size()}, decoded, consumed);
        dec_ec) {
        std::cerr << "[HSMS][ext][s9f1] S9F1 body 解码失败: " << dec_ec.message()
                  << "\n";
        session->stop();
        ::kill(proc.pid, SIGKILL);
        (void)co_await async_wait_exit(proc.pid, 3s);
        co_return 48;
    }
    if (consumed != rsp.body.size()) {
        std::cerr << "[HSMS][ext][s9f1] S9F1 body 未完全消费\n";
        session->stop();
        ::kill(proc.pid, SIGKILL);
        (void)co_await async_wait_exit(proc.pid, 3s);
        co_return 49;
    }
#
    const auto *bin = decoded.get_if<secs::ii::Binary>();
    if (!bin || bin->value.size() < 10) {
        std::cerr << "[HSMS][ext][s9f1] S9F1 item 非 Binary(>=10B)\n";
        session->stop();
        ::kill(proc.pid, SIGKILL);
        (void)co_await async_wait_exit(proc.pid, 3s);
        co_return 50;
    }
#
    const auto &hb = bin->value;
    const std::uint16_t dev =
        static_cast<std::uint16_t>((static_cast<std::uint16_t>(hb[0]) << 8U) |
                                   static_cast<std::uint16_t>(hb[1]));
    if (dev != local_device_id) {
        std::cerr << "[HSMS][ext][s9f1] header bytes DeviceId 不匹配: " << dev
                  << "\n";
        session->stop();
        ::kill(proc.pid, SIGKILL);
        (void)co_await async_wait_exit(proc.pid, 3s);
        co_return 51;
    }
    if (hb[2] != static_cast<byte>(0x80U | 0x01U) || hb[3] != 13) {
        std::cerr << "[HSMS][ext][s9f1] header bytes S/F/W 不匹配\n";
        session->stop();
        ::kill(proc.pid, SIGKILL);
        (void)co_await async_wait_exit(proc.pid, 3s);
        co_return 52;
    }
    if (hb[4] != 0 || hb[5] != 0) {
        std::cerr << "[HSMS][ext][s9f1] header bytes PType/MessageType 不匹配\n";
        session->stop();
        ::kill(proc.pid, SIGKILL);
        (void)co_await async_wait_exit(proc.pid, 3s);
        co_return 53;
    }
    const std::uint32_t id =
        (static_cast<std::uint32_t>(hb[6]) << 24U) |
        (static_cast<std::uint32_t>(hb[7]) << 16U) |
        (static_cast<std::uint32_t>(hb[8]) << 8U) |
        static_cast<std::uint32_t>(hb[9]);
    if (id != sb) {
        std::cerr << "[HSMS][ext][s9f1] header bytes system-bytes 不匹配\n";
        session->stop();
        ::kill(proc.pid, SIGKILL);
        (void)co_await async_wait_exit(proc.pid, 3s);
        co_return 54;
    }
#
    auto [wait_ec, exit_code] = co_await async_wait_exit(proc.pid, 10s);
    if (wait_ec || exit_code != 0) {
        std::cerr << "[HSMS][ext][s9f1] dotnet 返回码异常";
        if (wait_ec) {
            std::cerr << " wait=" << wait_ec.message();
        }
        std::cerr << " exit=" << exit_code << "\n";
        session->stop();
        ::kill(proc.pid, SIGKILL);
        (void)co_await async_wait_exit(proc.pid, 3s);
        co_return 55;
    }
#
    session->stop();
    co_return 0;
}
#
asio::awaitable<int> run_all() {
    if (const int rc = co_await case_large_payload(); rc != 0) {
        co_return rc;
    }
    if (const int rc = co_await case_out_of_order_responses(); rc != 0) {
        co_return rc;
    }
    if (const int rc = co_await case_device_id_mismatch_s9f1(); rc != 0) {
        co_return rc;
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
