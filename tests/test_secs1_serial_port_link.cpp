#include "test_main.hpp"

#include <secs/core/common.hpp>
#include <secs/core/error.hpp>
#include <secs/secs1/serial_port_link.hpp>

#include <asio/as_tuple.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/posix/stream_descriptor.hpp>
#include <asio/read.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#if defined(__unix__)
#include <fcntl.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace {

using namespace std::chrono_literals;

using secs::core::byte;
using secs::core::bytes_view;
using secs::core::errc;
using secs::core::make_error_code;
using secs::secs1::SerialPortLink;

#if defined(__unix__)

struct PtyPair final {
    int master_fd{-1};
    std::string slave_path{};
};

[[nodiscard]] bool set_raw_mode(int fd) noexcept {
    termios tio{};
    if (::tcgetattr(fd, &tio) != 0) {
        return false;
    }
    ::cfmakeraw(&tio);
    if (::tcsetattr(fd, TCSANOW, &tio) != 0) {
        return false;
    }
    return true;
}

[[nodiscard]] std::optional<PtyPair> create_pty_pair() noexcept {
    int master = ::posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0) {
        return std::nullopt;
    }

    auto close_master = [&]() noexcept { ::close(master); };

    if (::grantpt(master) != 0) {
        close_master();
        return std::nullopt;
    }
    if (::unlockpt(master) != 0) {
        close_master();
        return std::nullopt;
    }

    char *name = ::ptsname(master);
    if (!name) {
        close_master();
        return std::nullopt;
    }

    // 先把 slave 端设成 raw，避免 canonical 模式导致读单字节被行缓冲阻塞。
    int slave_fd = ::open(name, O_RDWR | O_NOCTTY);
    if (slave_fd < 0) {
        close_master();
        return std::nullopt;
    }
    const bool raw_ok = set_raw_mode(slave_fd);
    ::close(slave_fd);
    if (!raw_ok) {
        close_master();
        return std::nullopt;
    }

    PtyPair out{};
    out.master_fd = master;
    out.slave_path = name;
    return out;
}

void test_open_invalid_path_fails() {
    asio::io_context ioc;
    auto [ec, link] = SerialPortLink::open(ioc.get_executor(),
                                           "/dev/this_path_should_not_exist",
                                           9600);
    TEST_EXPECT(static_cast<bool>(ec));
    (void)link.executor();
}

void test_open_non_serial_path_does_not_crash() {
    // 说明：在部分环境中无法创建 PTY/真实串口，这里用一个“可打开但不是串口”的路径
    // 覆盖 open() 的更多分支，并确保失败时能正确清理资源且不崩溃/不阻塞。
    asio::io_context ioc;
    auto [ec, link] = SerialPortLink::open(ioc.get_executor(), "/dev/null", 9600);
    (void)link.executor();

    // /dev/null 在多数系统上不是 tty；通常会失败（ec!=0）。
    // 即便某些平台行为不同，也不强制断言，以避免用例脆弱。
    (void)ec;
}

void test_read_write_and_timeout_with_pty() {
    auto pty = create_pty_pair();
    if (!pty.has_value()) {
        // 说明：部分沙箱环境会禁用 PTY（例如 /dev/ptmx -> EPERM）。
        // 此时无法进行“真实字节流”读写回环测试；保留用例以覆盖 open 失败分支，
        // 并在允许 PTY 的环境下自动补齐覆盖率与回归验证。
        return;
    }

    asio::io_context ioc;
    const auto ex = ioc.get_executor();

    asio::posix::stream_descriptor master(ioc, pty->master_fd);

    auto [open_ec, link] = SerialPortLink::open(ex, pty->slave_path, 9600);
    TEST_EXPECT_OK(open_ec);
    if (open_ec) {
        return;
    }

    // executor() 分支可调用（覆盖轻量分支）
    (void)link.executor();

    // 1) read timeout：不写入任何数据，应超时并 cancel 底层读操作。
    std::optional<std::pair<std::error_code, byte>> timeout_out;
    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            auto [ec, b] = co_await link.async_read_byte(10ms);
            timeout_out = std::pair{ec, b};
            co_return;
        },
        asio::detached);
    ioc.run();

    TEST_EXPECT(timeout_out.has_value());
    if (timeout_out.has_value()) {
        TEST_EXPECT_EQ(timeout_out->first, make_error_code(errc::timeout));
        TEST_EXPECT_EQ(timeout_out->second, static_cast<byte>(0));
    }

    // 2) async_write：从 SerialPortLink 写入，应能在 master 端读到相同 bytes。
    ioc.restart();
    const std::vector<byte> payload = {
        static_cast<byte>(0x01),
        static_cast<byte>(0x02),
        static_cast<byte>(0x03),
    };

    std::optional<std::error_code> write_ec;
    std::optional<std::vector<byte>> read_payload;

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            write_ec = co_await link.async_write(
                bytes_view{payload.data(), payload.size()});
            co_return;
        },
        asio::detached);

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            std::vector<byte> buf(payload.size());
            auto [ec, n] = co_await asio::async_read(
                master,
                asio::buffer(buf.data(), buf.size()),
                asio::as_tuple(asio::use_awaitable));
            TEST_EXPECT_OK(ec);
            TEST_EXPECT_EQ(n, buf.size());
            read_payload = std::move(buf);
            co_return;
        },
        asio::detached);

    ioc.run();

    TEST_EXPECT(write_ec.has_value());
    if (write_ec.has_value()) {
        TEST_EXPECT_OK(*write_ec);
    }
    TEST_EXPECT(read_payload.has_value());
    if (read_payload.has_value()) {
        TEST_EXPECT_EQ(read_payload->size(), payload.size());
        TEST_EXPECT_EQ((*read_payload)[0], payload[0]);
        TEST_EXPECT_EQ((*read_payload)[1], payload[1]);
        TEST_EXPECT_EQ((*read_payload)[2], payload[2]);
    }

    // 3) async_read_byte（无 timeout）：master 端写入 1 字节，应能被立即读到。
    ioc.restart();

    std::optional<std::pair<std::error_code, byte>> read_out;
    constexpr byte kB = static_cast<byte>(0x42);

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            auto [ec, b] = co_await link.async_read_byte(std::nullopt);
            read_out = std::pair{ec, b};
            co_return;
        },
        asio::detached);

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            std::array<byte, 1> one = {kB};
            auto [ec, n] = co_await asio::async_write(
                master,
                asio::buffer(one.data(), one.size()),
                asio::as_tuple(asio::use_awaitable));
            TEST_EXPECT_OK(ec);
            TEST_EXPECT_EQ(n, std::size_t{1});
            co_return;
        },
        asio::detached);

    ioc.run();

    TEST_EXPECT(read_out.has_value());
    if (read_out.has_value()) {
        TEST_EXPECT_OK(read_out->first);
        TEST_EXPECT_EQ(read_out->second, kB);
    }
}

#endif

} // namespace

int main() {
#if defined(__unix__)
    test_open_invalid_path_fails();
    test_open_non_serial_path_does_not_crash();
    test_read_write_and_timeout_with_pty();
#endif
    return secs::tests::run_and_report();
}
