#include "secs/core/event.hpp"
#include "secs/core/common.hpp"
#include "secs/core/error.hpp"
#
#include "secs/protocol/session.hpp"
#include "secs/secs1/link.hpp"
#include "secs/secs1/state_machine.hpp"
#
#include <asio/as_tuple.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/posix/stream_descriptor.hpp>
#include <asio/read.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>
#
#include <fcntl.h>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>
#
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>
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
static std::error_code configure_tty_raw(int fd) noexcept {
    termios tio{};
    if (::tcgetattr(fd, &tio) != 0) {
        return make_errno_ec();
    }

    ::cfmakeraw(&tio);

    // 禁用软件流控（0x11/0x13 等控制字节会被吞掉，影响二进制协议）。
    tio.c_iflag &= static_cast<tcflag_t>(~(IXON | IXOFF | IXANY));

    // 关闭硬件流控（pty 不需要）。
#ifdef CRTSCTS
    tio.c_cflag &= static_cast<tcflag_t>(~CRTSCTS);
#endif

    // 确保本地模式与接收使能。
    tio.c_cflag |= static_cast<tcflag_t>(CLOCAL | CREAD);

    if (::tcsetattr(fd, TCSANOW, &tio) != 0) {
        return make_errno_ec();
    }
    (void)::tcflush(fd, TCIOFLUSH);
    return {};
}
#
// 创建一个 pty（master + slave path），用于后续把两个 pty “交叉桥接”成一根虚拟串口线。
static std::pair<std::error_code, std::pair<UniqueFd, std::string>>
create_pty() noexcept {
    errno = 0;
    const int master = ::posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (master < 0) {
        return {make_errno_ec(), std::pair<UniqueFd, std::string>{}};
    }
    UniqueFd master_fd(master);
#
    if (::grantpt(master_fd.fd) != 0) {
        return {make_errno_ec(), std::pair<UniqueFd, std::string>{}};
    }
    if (::unlockpt(master_fd.fd) != 0) {
        return {make_errno_ec(), std::pair<UniqueFd, std::string>{}};
    }
#
    errno = 0;
    const char *slave_name = ::ptsname(master_fd.fd);
    if (!slave_name) {
        return {make_errno_ec(), std::pair<UniqueFd, std::string>{}};
    }
#
    return {std::error_code{},
            std::pair<UniqueFd, std::string>{std::move(master_fd),
                                            std::string(slave_name)}};
}
#
// 打开 slave 端作为“串口设备”，用 stream_descriptor 做异步读写。
static std::pair<std::error_code, UniqueFd>
open_slave(const std::string &path) noexcept {
    errno = 0;
    const int fd = ::open(path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        return {make_errno_ec(), UniqueFd{}};
    }
    if (auto ec = configure_tty_raw(fd); ec) {
        ::close(fd);
        return {ec, UniqueFd{}};
    }
    return {std::error_code{}, UniqueFd(fd)};
}
#
static std::pair<std::error_code, std::pair<UniqueFd, UniqueFd>>
create_socketpair() noexcept {
    int fds[2]{-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        return {make_errno_ec(), std::pair<UniqueFd, UniqueFd>{}};
    }

    for (int i = 0; i < 2; ++i) {
        const int flags = ::fcntl(fds[i], F_GETFL, 0);
        if (flags < 0) {
            return {make_errno_ec(), std::pair<UniqueFd, UniqueFd>{}};
        }
        if (::fcntl(fds[i], F_SETFL, flags | O_NONBLOCK) != 0) {
            return {make_errno_ec(), std::pair<UniqueFd, UniqueFd>{}};
        }
    }

    return {std::error_code{},
            std::pair<UniqueFd, UniqueFd>{UniqueFd(fds[0]), UniqueFd(fds[1])}};
}
#
class PtyLink final : public secs::secs1::Link {
public:
    explicit PtyLink(asio::posix::stream_descriptor sd)
        : ex_(sd.get_executor()), sd_(std::move(sd)) {}
#
    [[nodiscard]] asio::any_io_executor executor() const noexcept override {
        return ex_;
    }
#
    asio::awaitable<std::error_code>
    async_write(secs::core::bytes_view data) override {
        std::size_t total = 0;
        while (total < data.size()) {
            auto [ec, n] = co_await asio::async_write(
                sd_,
                asio::buffer(data.data() + total, data.size() - total),
                asio::as_tuple(asio::use_awaitable));
            if (ec) {
                co_return ec;
            }
            total += n;
        }
        co_return std::error_code{};
    }
#
    asio::awaitable<std::pair<std::error_code, secs::core::byte>>
    async_read_byte(std::optional<secs::core::duration> timeout) override {
        secs::core::byte b = 0;
        std::error_code read_ec{};
        std::size_t read_n = 0;
        secs::core::Event done{};
#
        asio::co_spawn(
            sd_.get_executor(),
            [&]() -> asio::awaitable<void> {
                auto [ec, n] = co_await asio::async_read(
                    sd_,
                    asio::buffer(&b, 1),
                    asio::as_tuple(asio::use_awaitable));
                read_ec = ec;
                read_n = n;
                done.set();
            },
            asio::detached);
#
        const auto wait_ec = co_await done.async_wait(timeout);
        if (wait_ec == secs::core::make_error_code(secs::core::errc::timeout)) {
            sd_.cancel();
            (void)co_await done.async_wait(1s);
            co_return std::pair{wait_ec, secs::core::byte{0}};
        }
        if (wait_ec) {
            sd_.cancel();
            (void)co_await done.async_wait(1s);
            co_return std::pair{wait_ec, secs::core::byte{0}};
        }
#
        if (read_ec) {
            co_return std::pair{read_ec, secs::core::byte{0}};
        }
        if (read_n != 1) {
            co_return std::pair{
                secs::core::make_error_code(secs::core::errc::invalid_argument),
                secs::core::byte{0}};
        }
        co_return std::pair{std::error_code{}, b};
    }
#
private:
    asio::any_io_executor ex_{};
    asio::posix::stream_descriptor sd_;
};
#
asio::awaitable<void> bridge_pump(asio::posix::stream_descriptor &from,
                                  asio::posix::stream_descriptor &to) {
    std::vector<byte> buf(4096);
    for (;;) {
        auto [ec, n] = co_await from.async_read_some(
            asio::buffer(buf.data(), buf.size()),
            asio::as_tuple(asio::use_awaitable));
        if (ec) {
            break;
        }
#
        std::size_t written = 0;
        while (written < n) {
            auto [wec, wn] = co_await asio::async_write(
                to,
                asio::buffer(buf.data() + written, n - written),
                asio::as_tuple(asio::use_awaitable));
            if (wec) {
                co_return;
            }
            written += wn;
        }
    }
}
#
asio::awaitable<int> run() {
    auto ex = co_await asio::this_coro::executor;
#
    // 1) 构造“真实字节流链路”
    //
    // - 优先：pty（在真实 Linux 环境可模拟两端“串口设备”）
    // - 退化：socketpair（在沙箱/受限环境中常常没有 /dev/ptmx 权限）
    std::optional<asio::posix::stream_descriptor> master_a_opt;
    std::optional<asio::posix::stream_descriptor> master_b_opt;
    std::optional<asio::posix::stream_descriptor> end_a_opt;
    std::optional<asio::posix::stream_descriptor> end_b_opt;
#
    auto [pty1_ec, pty1] = create_pty();
    auto [pty2_ec, pty2] = create_pty();
    if (!pty1_ec && !pty2_ec) {
        auto [slave1_ec, slave1_fd] = open_slave(pty1.second);
        auto [slave2_ec, slave2_fd] = open_slave(pty2.second);
        if (!slave1_ec && !slave2_ec) {
            master_a_opt.emplace(ex, pty1.first.fd);
            master_b_opt.emplace(ex, pty2.first.fd);
            pty1.first.fd = -1;
            pty2.first.fd = -1;
#
            end_a_opt.emplace(ex, slave1_fd.fd);
            end_b_opt.emplace(ex, slave2_fd.fd);
            slave1_fd.fd = -1;
            slave2_fd.fd = -1;
#
            asio::co_spawn(ex, bridge_pump(*master_a_opt, *master_b_opt),
                           asio::detached);
            asio::co_spawn(ex, bridge_pump(*master_b_opt, *master_a_opt),
                           asio::detached);
        }
    }

#ifdef SECS_SECS1_REQUIRE_PTY
    if (!end_a_opt.has_value() || !end_b_opt.has_value()) {
        std::cerr << "[SECS-I] pty 不可用，跳过\n";
        co_return 77;
    }
#endif
#
    if (!end_a_opt.has_value() || !end_b_opt.has_value()) {
        auto [sp_ec, sp] = create_socketpair();
        if (sp_ec) {
            std::cerr << "创建 socketpair 失败: " << sp_ec.message() << "\n";
            co_return 2;
        }
        end_a_opt.emplace(ex, sp.first.fd);
        end_b_opt.emplace(ex, sp.second.fd);
        sp.first.fd = -1;
        sp.second.fd = -1;
    }
#
    // 2) 在两个“串口端”上各自运行一个 SECS-I StateMachine + protocol::Session
    //    用例目标：验证真实字节流 + 分包/重组 + 请求-响应可用。
    PtyLink link_a(std::move(*end_a_opt));
    PtyLink link_b(std::move(*end_b_opt));
#
    constexpr std::uint16_t device_id = 1;
#
    secs::secs1::StateMachine sm_a(link_a, device_id);
    secs::secs1::StateMachine sm_b(link_b, device_id);
#
    secs::protocol::SessionOptions opt_a{};
    opt_a.t3 = 3s;
    opt_a.poll_interval = 20ms;
    opt_a.secs1_reverse_bit = false; // Host -> Equipment
#
    secs::protocol::SessionOptions opt_b = opt_a;
    opt_b.secs1_reverse_bit = true; // Equipment -> Host
#
    secs::protocol::Session sess_a(sm_a, device_id, opt_a);
    secs::protocol::Session sess_b(sm_b, device_id, opt_b);
#
    sess_b.router().set(
        1,
        13,
        [](const secs::protocol::DataMessage &msg)
            -> asio::awaitable<secs::protocol::HandlerResult> {
            co_return secs::protocol::HandlerResult{std::error_code{}, msg.body};
        });
#
    asio::co_spawn(
        ex,
        [&]() -> asio::awaitable<void> {
            co_await sess_b.async_run();
        },
        asio::detached);
#
    // 3) 构造 >244B 的 payload，触发多块分包与重组。
    std::vector<byte> payload;
    payload.resize(700);
    for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<byte>(i & 0xFFU);
    }
#
    auto [ec, rsp] = co_await sess_a.async_request(
        1, 13, bytes_view{payload.data(), payload.size()}, 5s);
    if (ec) {
        std::cerr << "SECS-I 请求-响应失败: " << ec.message() << "\n";
        sess_a.stop();
        sess_b.stop();
        co_return 6;
    }
    if (rsp.stream != 1 || rsp.function != 14 || rsp.w_bit) {
        std::cerr << "SECS-I 响应头不符合预期\n";
        sess_a.stop();
        sess_b.stop();
        co_return 7;
    }
    if (rsp.body != payload) {
        std::cerr << "SECS-I 回显 payload 不一致\n";
        sess_a.stop();
        sess_b.stop();
        co_return 8;
    }
#
    sess_a.stop();
    sess_b.stop();
    if (master_a_opt.has_value()) {
        master_a_opt->close();
    }
    if (master_b_opt.has_value()) {
        master_b_opt->close();
    }
#
    std::cout << "PASS\n";
    co_return 0;
}
#
} // namespace
#
int main() {
    asio::io_context io;
    int rc = 1;
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            rc = co_await run();
            io.stop();
        },
        asio::detached);
    io.run();
    return rc;
}
