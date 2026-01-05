#include "secs/core/common.hpp"
#include "secs/core/error.hpp"
#
#include "secs/hsms/connection.hpp"
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
#include <asio/posix/stream_descriptor.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>
#
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#
#include <algorithm>
#include <chrono>
#include <cerrno>
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
using secs::core::mutable_bytes_view;
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
struct UniqueFd final {
    int fd{-1};
    UniqueFd() = default;
    explicit UniqueFd(int v) : fd(v) {}
    UniqueFd(const UniqueFd &) = delete;
    UniqueFd &operator=(const UniqueFd &) = delete;
    UniqueFd(UniqueFd &&o) noexcept : fd(o.fd) { o.fd = -1; }
    UniqueFd &operator=(UniqueFd &&o) noexcept {
        if (this == &o) {
            return *this;
        }
        reset();
        fd = o.fd;
        o.fd = -1;
        return *this;
    }
    ~UniqueFd() { reset(); }
    void reset() noexcept {
        if (fd >= 0) {
            ::close(fd);
        }
        fd = -1;
    }
};
#
static std::error_code make_errno_ec() noexcept {
    return std::error_code(errno, std::generic_category());
}
#
static std::error_code set_nonblocking(int fd) noexcept {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return make_errno_ec();
    }
    if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        return make_errno_ec();
    }
    return {};
}
#
struct ChildProcess final {
    pid_t pid{-1};
    UniqueFd stdin_write{};
    UniqueFd stdout_read{};
};
#
static std::pair<std::error_code, ChildProcess>
spawn_dotnet_peer(std::string_view mode, std::uint16_t device_id) noexcept {
    int in_pipe[2]{-1, -1};  // parent -> child (stdin)
    int out_pipe[2]{-1, -1}; // child -> parent (stdout)
#
    if (::pipe(in_pipe) != 0) {
        return {make_errno_ec(), ChildProcess{}};
    }
    if (::pipe(out_pipe) != 0) {
        ::close(in_pipe[0]);
        ::close(in_pipe[1]);
        return {make_errno_ec(), ChildProcess{}};
    }
#
    // 只需要父进程侧的 fd 非阻塞；子进程用阻塞 IO 更简单。
    if (auto ec = set_nonblocking(in_pipe[1]); ec) {
        ::close(in_pipe[0]);
        ::close(in_pipe[1]);
        ::close(out_pipe[0]);
        ::close(out_pipe[1]);
        return {ec, ChildProcess{}};
    }
    if (auto ec = set_nonblocking(out_pipe[0]); ec) {
        ::close(in_pipe[0]);
        ::close(in_pipe[1]);
        ::close(out_pipe[0]);
        ::close(out_pipe[1]);
        return {ec, ChildProcess{}};
    }
#
    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(in_pipe[0]);
        ::close(in_pipe[1]);
        ::close(out_pipe[0]);
        ::close(out_pipe[1]);
        return {make_errno_ec(), ChildProcess{}};
    }
#
    if (pid == 0) {
        // child
        (void)::dup2(in_pipe[0], STDIN_FILENO);
        (void)::dup2(out_pipe[1], STDOUT_FILENO);
#
        ::close(in_pipe[0]);
        ::close(in_pipe[1]);
        ::close(out_pipe[0]);
        ::close(out_pipe[1]);
#
        // 避免 dotnet host 输出任何 banner 到 stdout（stdout 被用作 HSMS 二进制流）。
        (void)::setenv("DOTNET_SKIP_FIRST_TIME_EXPERIENCE", "1", 1);
        (void)::setenv("DOTNET_CLI_TELEMETRY_OPTOUT", "1", 1);
        (void)::setenv("DOTNET_NOLOGO", "1", 1);
#
        const std::string device_id_str = std::to_string(device_id);
        const std::string mode_str(mode);
#
        std::vector<std::string> argv_s{
            std::string(SECS_HSMS_DOTNET_EXECUTABLE),
            std::string(SECS_HSMS_PEER_DLL),
            "--mode",
            mode_str,
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
        // exec 失败
        std::cerr << "execvp(dotnet) 失败: " << std::strerror(errno) << "\n";
        std::_Exit(127);
    }
#
    // parent
    ::close(in_pipe[0]);
    ::close(out_pipe[1]);
#
    ChildProcess proc{};
    proc.pid = pid;
    proc.stdin_write = UniqueFd(in_pipe[1]);
    proc.stdout_read = UniqueFd(out_pipe[0]);
    return {std::error_code{}, std::move(proc)};
}
#
class PipeStream final : public secs::hsms::Stream {
public:
    PipeStream(asio::any_io_executor ex,
               int read_fd,
               int write_fd,
               std::size_t max_read_chunk,
               std::size_t max_write_chunk)
        : ex_(ex),
          in_(ex, read_fd),
          out_(ex, write_fd),
          max_read_chunk_(std::max<std::size_t>(1, max_read_chunk)),
          max_write_chunk_(std::max<std::size_t>(1, max_write_chunk)) {
        std::error_code ignored;
        in_.non_blocking(true, ignored);
        out_.non_blocking(true, ignored);
    }
#
    [[nodiscard]] asio::any_io_executor executor() const noexcept override {
        return ex_;
    }
    [[nodiscard]] bool is_open() const noexcept override {
        return in_.is_open() && out_.is_open();
    }
#
    void cancel() noexcept override {
        std::error_code ignored;
        in_.cancel(ignored);
        out_.cancel(ignored);
    }
#
    void close() noexcept override {
        std::error_code ignored;
        in_.close(ignored);
        out_.close(ignored);
    }
#
    asio::awaitable<std::pair<std::error_code, std::size_t>>
    async_read_some(mutable_bytes_view dst) override {
        const auto n = std::min<std::size_t>(dst.size(), max_read_chunk_);
        auto [ec, got] = co_await in_.async_read_some(
            asio::buffer(dst.data(), n), asio::as_tuple(asio::use_awaitable));
        co_return std::pair{ec, got};
    }
#
    asio::awaitable<std::error_code> async_write_all(bytes_view src) override {
        std::size_t offset = 0;
        while (offset < src.size()) {
            const auto chunk =
                std::min<std::size_t>(src.size() - offset, max_write_chunk_);
            auto [ec, n] = co_await asio::async_write(
                out_,
                asio::buffer(src.data() + offset, chunk),
                asio::as_tuple(asio::use_awaitable));
            if (ec) {
                co_return ec;
            }
            if (n != chunk) {
                co_return secs::core::make_error_code(
                    secs::core::errc::invalid_argument);
            }
            offset += n;
        }
        co_return std::error_code{};
    }
#
    asio::awaitable<std::error_code>
    async_connect(const asio::ip::tcp::endpoint &) override {
        co_return secs::core::make_error_code(secs::core::errc::invalid_argument);
    }
#
private:
    asio::any_io_executor ex_{};
    asio::posix::stream_descriptor in_;
    asio::posix::stream_descriptor out_;
    std::size_t max_read_chunk_{1};
    std::size_t max_write_chunk_{1};
};
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
            co_return std::pair{secs::core::make_error_code(secs::core::errc::timeout),
                                -1};
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
#
    auto [spawn_ec, proc] = spawn_dotnet_peer("pipe-echo-server", device_id);
    if (spawn_ec) {
        std::cerr << "[HSMS][case1] 启动 dotnet 失败: " << spawn_ec.message()
                  << "\n";
        co_return 2;
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
#
    auto stream = std::make_unique<PipeStream>(ex,
                                               proc.stdout_read.fd,
                                               proc.stdin_write.fd,
                                               /*max_read_chunk=*/37,
                                               /*max_write_chunk=*/23);
    proc.stdout_read.fd = -1;
    proc.stdin_write.fd = -1;
#
    secs::hsms::Connection conn(
        std::move(stream), secs::hsms::ConnectionOptions{.t8 = options.t8});
#
    if (auto ec = co_await session.async_open_active(std::move(conn)); ec) {
        std::cerr << "[HSMS][case1] C++ active SELECT 失败: " << ec.message()
                  << "\n";
        ::kill(proc.pid, SIGKILL);
        auto [wait_ec, exit_code] = co_await async_wait_exit(proc.pid, 3s);
        (void)wait_ec;
        (void)exit_code;
        co_return 3;
    }
#
    if (auto ec = co_await session.async_linktest(); ec) {
        std::cerr << "[HSMS][case1] LINKTEST 失败: " << ec.message() << "\n";
        session.stop();
        ::kill(proc.pid, SIGKILL);
        auto [wait_ec, exit_code] = co_await async_wait_exit(proc.pid, 3s);
        (void)wait_ec;
        (void)exit_code;
        co_return 4;
    }
#
    const auto item = make_test_item();
    std::vector<byte> body;
    if (auto enc_ec = secs::ii::encode(item, body); enc_ec) {
        std::cerr << "[HSMS][case1] SECS-II 编码失败: " << enc_ec.message()
                  << "\n";
        session.stop();
        ::kill(proc.pid, SIGKILL);
        auto [wait_ec, exit_code] = co_await async_wait_exit(proc.pid, 3s);
        (void)wait_ec;
        (void)exit_code;
        co_return 5;
    }
#
    auto [req_ec, rsp] = co_await session.async_request_data(
        /*stream=*/1,
        /*function=*/13,
        bytes_view{body.data(), body.size()},
        5s);
    if (req_ec) {
        std::cerr << "[HSMS][case1] 请求-响应失败: " << req_ec.message() << "\n";
        session.stop();
        ::kill(proc.pid, SIGKILL);
        auto [wait_ec, exit_code] = co_await async_wait_exit(proc.pid, 3s);
        (void)wait_ec;
        (void)exit_code;
        co_return 6;
    }
#
    if (rsp.stream() != 1 || rsp.function() != 14 || rsp.w_bit()) {
        std::cerr << "[HSMS][case1] 响应头不符合预期: stream="
                  << static_cast<int>(rsp.stream())
                  << " function=" << static_cast<int>(rsp.function())
                  << " w=" << (rsp.w_bit() ? 1 : 0) << "\n";
        session.stop();
        ::kill(proc.pid, SIGKILL);
        auto [wait_ec, exit_code] = co_await async_wait_exit(proc.pid, 3s);
        (void)wait_ec;
        (void)exit_code;
        co_return 7;
    }
#
    secs::ii::Item decoded = secs::ii::Item::binary({});
    std::size_t consumed = 0;
    if (auto dec_ec = secs::ii::decode_one(
            bytes_view{rsp.body.data(), rsp.body.size()}, decoded, consumed);
        dec_ec) {
        std::cerr << "[HSMS][case1] 响应 body 解码失败: " << dec_ec.message()
                  << "\n";
        session.stop();
        ::kill(proc.pid, SIGKILL);
        auto [wait_ec, exit_code] = co_await async_wait_exit(proc.pid, 3s);
        (void)wait_ec;
        (void)exit_code;
        co_return 8;
    }
    if (consumed != rsp.body.size()) {
        std::cerr << "[HSMS][case1] 响应 body 存在未消费尾部: consumed="
                  << consumed << " total=" << rsp.body.size() << "\n";
        session.stop();
        ::kill(proc.pid, SIGKILL);
        auto [wait_ec, exit_code] = co_await async_wait_exit(proc.pid, 3s);
        (void)wait_ec;
        (void)exit_code;
        co_return 9;
    }
    if (decoded != item) {
        std::cerr << "[HSMS][case1] 响应 Item 与请求不一致\n";
        session.stop();
        ::kill(proc.pid, SIGKILL);
        auto [wait_ec, exit_code] = co_await async_wait_exit(proc.pid, 3s);
        (void)wait_ec;
        (void)exit_code;
        co_return 10;
    }
#
    session.stop();
#
    auto [wait_ec, exit_code] = co_await async_wait_exit(proc.pid, 5s);
    if (wait_ec) {
        std::cerr << "[HSMS][case1] 等待 dotnet 退出失败: " << wait_ec.message()
                  << "\n";
        ::kill(proc.pid, SIGKILL);
        (void)co_await async_wait_exit(proc.pid, 3s);
        co_return 11;
    }
    if (exit_code != 0) {
        std::cerr << "[HSMS][case1] dotnet 返回码异常: " << exit_code << "\n";
        co_return 12;
    }
#
    co_return 0;
}
#
static asio::awaitable<int> run_case_dotnet_active_cpp_passive() {
    auto ex = co_await asio::this_coro::executor;
    constexpr std::uint16_t device_id = 1;
#
    auto [spawn_ec, proc] = spawn_dotnet_peer("pipe-request-client", device_id);
    if (spawn_ec) {
        std::cerr << "[HSMS][case2] 启动 dotnet 失败: " << spawn_ec.message()
                  << "\n";
        co_return 20;
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
#
    auto stream = std::make_unique<PipeStream>(ex,
                                               proc.stdout_read.fd,
                                               proc.stdin_write.fd,
                                               /*max_read_chunk=*/41,
                                               /*max_write_chunk=*/19);
    proc.stdout_read.fd = -1;
    proc.stdin_write.fd = -1;
#
    secs::hsms::Connection conn(
        std::move(stream), secs::hsms::ConnectionOptions{.t8 = options.t8});
#
    if (auto ec = co_await session.async_open_passive(std::move(conn)); ec) {
        std::cerr << "[HSMS][case2] C++ passive SELECT 等待失败: " << ec.message()
                  << "\n";
        ::kill(proc.pid, SIGKILL);
        (void)co_await async_wait_exit(proc.pid, 3s);
        co_return 21;
    }
#
    auto [rec_ec, req] = co_await session.async_receive_data(5s);
    if (rec_ec) {
        std::cerr << "[HSMS][case2] 接收 data message 失败: " << rec_ec.message()
                  << "\n";
        session.stop();
        ::kill(proc.pid, SIGKILL);
        (void)co_await async_wait_exit(proc.pid, 3s);
        co_return 22;
    }
#
    secs::ii::Item decoded = secs::ii::Item::binary({});
    std::size_t consumed = 0;
    if (auto dec_ec = secs::ii::decode_one(
            bytes_view{req.body.data(), req.body.size()}, decoded, consumed);
        dec_ec) {
        std::cerr << "[HSMS][case2] 请求 body 解码失败: " << dec_ec.message()
                  << "\n";
        session.stop();
        ::kill(proc.pid, SIGKILL);
        (void)co_await async_wait_exit(proc.pid, 3s);
        co_return 23;
    }
    if (consumed != req.body.size()) {
        std::cerr << "[HSMS][case2] 请求 body 存在未消费尾部: consumed="
                  << consumed << " total=" << req.body.size() << "\n";
        session.stop();
        ::kill(proc.pid, SIGKILL);
        (void)co_await async_wait_exit(proc.pid, 3s);
        co_return 24;
    }
#
    const auto expected = make_test_item();
    if (decoded != expected) {
        std::cerr << "[HSMS][case2] 入站 Item 与预期不一致\n";
        session.stop();
        ::kill(proc.pid, SIGKILL);
        (void)co_await async_wait_exit(proc.pid, 3s);
        co_return 25;
    }
#
    // 使用 C++ 编码结果回包；dotnet 会做 byte-level 比对，可覆盖双方编码一致性。
    std::vector<byte> encoded;
    if (auto enc_ec = secs::ii::encode(decoded, encoded); enc_ec) {
        std::cerr << "[HSMS][case2] 回包 SECS-II 编码失败: " << enc_ec.message()
                  << "\n";
        session.stop();
        ::kill(proc.pid, SIGKILL);
        (void)co_await async_wait_exit(proc.pid, 3s);
        co_return 26;
    }
    if (encoded.size() != req.body.size() ||
        !std::equal(encoded.begin(), encoded.end(), req.body.begin())) {
        std::cerr << "[HSMS][case2] C++ 编码 bytes 与 dotnet 请求 bytes 不一致\n";
        session.stop();
        ::kill(proc.pid, SIGKILL);
        (void)co_await async_wait_exit(proc.pid, 3s);
        co_return 27;
    }
#
    const auto rsp = secs::hsms::make_data_message(
        options.session_id,
        req.stream(),
        static_cast<std::uint8_t>(req.function() + 1U),
        false,
        req.header.system_bytes,
        bytes_view{encoded.data(), encoded.size()});
#
    if (auto send_ec = co_await session.async_send(rsp); send_ec) {
        std::cerr << "[HSMS][case2] 发送回包失败: " << send_ec.message() << "\n";
        session.stop();
        ::kill(proc.pid, SIGKILL);
        (void)co_await async_wait_exit(proc.pid, 3s);
        co_return 28;
    }
#
    session.stop();
#
    auto [wait_ec, exit_code] = co_await async_wait_exit(proc.pid, 5s);
    if (wait_ec) {
        std::cerr << "[HSMS][case2] 等待 dotnet 退出失败: " << wait_ec.message()
                  << "\n";
        ::kill(proc.pid, SIGKILL);
        (void)co_await async_wait_exit(proc.pid, 3s);
        co_return 29;
    }
    if (exit_code != 0) {
        std::cerr << "[HSMS][case2] dotnet 返回码异常: " << exit_code << "\n";
        co_return 30;
    }
#
    co_return 0;
}
#
asio::awaitable<int> run_all() {
    // 用例 1：C++ 主动端 -> dotnet 被动端（含 LINKTEST + 请求响应）
    if (const int rc = co_await run_case_cpp_active_dotnet_passive(); rc != 0) {
        co_return rc;
    }
#
    // 用例 2：dotnet 主动端 -> C++ 被动端（含 LINKTEST.req 覆盖 + 请求响应）
    if (const int rc = co_await run_case_dotnet_active_cpp_passive(); rc != 0) {
        co_return rc;
    }
#
    co_return 0;
}
#
} // namespace
#
int main() {
    // 避免写入断管触发 SIGPIPE 终止进程（pipe 用例必需）。
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
