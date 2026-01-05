#pragma once

// 该文件仅用于 examples：提供一个基于 POSIX tty/pty 的 SECS-I Link 实现。
//
// 设计目标：
// - 让示例程序可以直接打开 `/dev/tty*` 或 `/dev/pts/*` 并运行 SECS-I；
// - 对虚拟串口（pty）默认开启 raw 模式与禁用 XON/XOFF，避免控制字节被吞/回显；
// - 不把平台相关实现引入库公共 API，保持库本体的可移植性。

#include "secs/core/event.hpp"
#include "secs/secs1/link.hpp"

#include <asio/as_tuple.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/posix/stream_descriptor.hpp>
#include <asio/read.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>

#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

namespace secs::examples {

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

    void reset(int new_fd = -1) noexcept {
        if (fd >= 0) {
            ::close(fd);
        }
        fd = new_fd;
    }

    [[nodiscard]] int release() noexcept {
        const int out = fd;
        fd = -1;
        return out;
    }

    [[nodiscard]] bool valid() const noexcept { return fd >= 0; }
};

[[nodiscard]] inline std::error_code make_errno_ec() noexcept {
    return std::error_code(errno, std::generic_category());
}

[[nodiscard]] inline std::optional<speed_t> baud_to_speed(int baud) noexcept {
    switch (baud) {
    case 1200:
        return B1200;
    case 2400:
        return B2400;
    case 4800:
        return B4800;
    case 9600:
        return B9600;
    case 19200:
        return B19200;
    case 38400:
        return B38400;
#ifdef B57600
    case 57600:
        return B57600;
#endif
#ifdef B115200
    case 115200:
        return B115200;
#endif
#ifdef B230400
    case 230400:
        return B230400;
#endif
    default:
        return std::nullopt;
    }
}

// 配置 tty 为 raw，禁用软件流控（XON/XOFF）与硬件流控；可选配置 baud。
[[nodiscard]] inline std::error_code configure_tty_raw(int fd,
                                                       int baud) noexcept {
    termios tio{};
    if (::tcgetattr(fd, &tio) != 0) {
        return make_errno_ec();
    }

    ::cfmakeraw(&tio);

    // 禁用软件流控（0x11/0x13 等控制字节会被吞掉，影响二进制协议）。
    tio.c_iflag &= static_cast<tcflag_t>(~(IXON | IXOFF | IXANY));

    // 关闭硬件流控（大多数虚拟串口不需要）。
#ifdef CRTSCTS
    tio.c_cflag &= static_cast<tcflag_t>(~CRTSCTS);
#endif

    // 确保本地模式与接收使能。
    tio.c_cflag |= static_cast<tcflag_t>(CLOCAL | CREAD);

    if (baud > 0) {
        const auto speed_opt = baud_to_speed(baud);
        if (!speed_opt.has_value()) {
            return std::make_error_code(std::errc::invalid_argument);
        }
        ::cfsetispeed(&tio, *speed_opt);
        ::cfsetospeed(&tio, *speed_opt);
    }

    if (::tcsetattr(fd, TCSANOW, &tio) != 0) {
        return make_errno_ec();
    }

    (void)::tcflush(fd, TCIOFLUSH);
    return {};
}

[[nodiscard]] inline std::pair<std::error_code, UniqueFd>
open_tty_raw(const std::string &path, int baud) noexcept {
    errno = 0;
    const int fd = ::open(path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        return {make_errno_ec(), UniqueFd{}};
    }

    if (auto ec = configure_tty_raw(fd, baud); ec) {
        ::close(fd);
        return {ec, UniqueFd{}};
    }

    return {std::error_code{}, UniqueFd(fd)};
}

class PosixSerialLink final : public secs::secs1::Link {
public:
    PosixSerialLink(asio::any_io_executor ex, UniqueFd fd)
        : ex_(ex), sd_(ex, fd.release()) {}

    [[nodiscard]] asio::any_io_executor executor() const noexcept override {
        return ex_;
    }

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

    asio::awaitable<std::pair<std::error_code, secs::core::byte>>
    async_read_byte(std::optional<secs::core::duration> timeout) override {
        using secs::core::errc;
        using secs::core::make_error_code;

        secs::core::byte b = 0;
        std::error_code read_ec{};
        std::size_t read_n = 0;
        secs::core::Event done{};

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

        const auto wait_ec = co_await done.async_wait(timeout);
        if (wait_ec == make_error_code(errc::timeout)) {
            sd_.cancel();
            (void)co_await done.async_wait(std::chrono::seconds(1));
            co_return std::pair{wait_ec, secs::core::byte{0}};
        }
        if (wait_ec) {
            sd_.cancel();
            (void)co_await done.async_wait(std::chrono::seconds(1));
            co_return std::pair{wait_ec, secs::core::byte{0}};
        }

        if (read_ec) {
            co_return std::pair{read_ec, secs::core::byte{0}};
        }
        if (read_n != 1) {
            co_return std::pair{make_error_code(errc::invalid_argument),
                                secs::core::byte{0}};
        }
        co_return std::pair{std::error_code{}, b};
    }

private:
    asio::any_io_executor ex_{};
    asio::posix::stream_descriptor sd_;
};

} // namespace secs::examples
