#pragma once

/*
 * 跨平台串口 Link：基于 standalone Asio 的 asio::serial_port。
 *
 * 适用场景：
 * - Windows：配合 com0com 创建的虚拟串口（COMx）做 SECS-I 联调/回归；
 * - POSIX：真实串口设备（/dev/ttyS* /dev/ttyUSB* 等）。
 *
 * 说明：
 * - 若你在 POSIX 上需要 pty/raw 模式（termios 细粒度控制），可继续使用
 *   PosixSerialLink（posix_serial_link.hpp）。
 */

#include <asio/detail/config.hpp>

#if !defined(ASIO_HAS_SERIAL_PORT)
#error "secs::secs1::SerialPortLink 需要 ASIO_HAS_SERIAL_PORT"
#endif

#include "secs/core/error.hpp"
#include "secs/secs1/link.hpp"

#include <asio/as_tuple.hpp>
#include <asio/co_spawn.hpp>
#include <asio/deferred.hpp>
#include <asio/experimental/parallel_group.hpp>
#include <asio/read.hpp>
#include <asio/serial_port.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>

#include <cctype>
#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace secs::serial {
namespace detail {

// Windows COM10+ 需要 \\.\COM10 形式；这里做一个宽松的规范化：
// - 已经是 \\.\ 或 \\?\ 前缀：原样返回
// - 形如 COM<number>（大小写不敏感）：补上 \\.\ 前缀
// - 其他：原样返回
[[nodiscard]] inline std::string
normalize_serial_port_path(std::string_view path) {
#if defined(_WIN32)
    if (path.size() >= 4 && (path.substr(0, 4) == R"(\\.\)" ||
                             path.substr(0, 4) == R"(\\?\)")) {
        return std::string(path);
    }

    auto ieq = [](char a, char b) noexcept {
        return static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(a))) ==
               static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(b)));
    };

    if (path.size() >= 4 && ieq(path[0], 'c') && ieq(path[1], 'o') &&
        ieq(path[2], 'm')) {
        bool all_digits = true;
        for (std::size_t i = 3; i < path.size(); ++i) {
            if (!std::isdigit(static_cast<unsigned char>(path[i]))) {
                all_digits = false;
                break;
            }
        }
        if (all_digits) {
            return std::string(R"(\\.\)") + std::string(path);
        }
    }
#endif
    return std::string(path);
}

} // namespace detail
} // namespace secs::serial

namespace secs::secs1 {

/**
 * @brief 基于 asio::serial_port 的 SECS-I Link。
 *
 * 注意：
 * - 该 Link 只提供“字节流读写”，不做任何协议层假设；
 * - async_read_byte 支持超时：超时会 cancel 底层 serial_port 的读操作。
 */
class SerialPortLink final : public Link {
public:
    // 创建一个“无效”的 Link（仅用于 open() 失败时作为占位返回值）。
    explicit SerialPortLink(asio::any_io_executor ex) : ex_(ex), port_(ex) {}

    // 便捷打开：open + 基础串口参数（失败时返回 ec，且 out link 为无效占位）。
    [[nodiscard]] static std::pair<std::error_code, SerialPortLink>
    open(asio::any_io_executor ex,
         const std::string &path,
         int baud) noexcept {
        SerialPortLink link(ex);

        std::error_code ec;
        const auto normalized = secs::serial::detail::normalize_serial_port_path(path);
        link.port_.open(normalized, ec);
        if (ec) {
            return {ec, SerialPortLink(ex)};
        }

        auto close_on_error = [&]() noexcept {
            std::error_code ignored;
            link.port_.cancel(ignored);
            link.port_.close(ignored);
        };

        // 基础配置：8N1 + 无流控（尽可能贴近“原始字节流”）。
        link.port_.set_option(asio::serial_port_base::character_size(8), ec);
        if (ec) {
            close_on_error();
            return {ec, SerialPortLink(ex)};
        }
        link.port_.set_option(asio::serial_port_base::parity(
                                  asio::serial_port_base::parity::none),
                              ec);
        if (ec) {
            close_on_error();
            return {ec, SerialPortLink(ex)};
        }
        link.port_.set_option(
            asio::serial_port_base::stop_bits(asio::serial_port_base::stop_bits::one),
            ec);
        if (ec) {
            close_on_error();
            return {ec, SerialPortLink(ex)};
        }
        link.port_.set_option(asio::serial_port_base::flow_control(
                                  asio::serial_port_base::flow_control::none),
                              ec);
        if (ec) {
            close_on_error();
            return {ec, SerialPortLink(ex)};
        }

        if (baud > 0) {
            link.port_.set_option(asio::serial_port_base::baud_rate(baud), ec);
            if (ec) {
                close_on_error();
                return {ec, SerialPortLink(ex)};
            }
        }

        return {std::error_code{}, std::move(link)};
    }

    [[nodiscard]] asio::any_io_executor executor() const noexcept override {
        return ex_;
    }

    asio::awaitable<std::error_code>
    async_write(secs::core::bytes_view data) override {
        std::size_t total = 0;
        while (total < data.size()) {
            auto [ec, n] = co_await asio::async_write(
                port_,
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
        if (!timeout.has_value()) {
            auto [ec, n] = co_await asio::async_read(
                port_,
                asio::buffer(&b, 1),
                asio::as_tuple(asio::use_awaitable));
            if (ec) {
                co_return std::pair{ec, secs::core::byte{0}};
            }
            if (n != 1) {
                co_return std::pair{make_error_code(errc::invalid_argument),
                                    secs::core::byte{0}};
            }
            co_return std::pair{std::error_code{}, b};
        }

        // 采用“并行等待 read 与 timer”的方式实现超时，避免 detached 协程捕获栈引用
        // 引发的生命周期问题。
        auto ex = port_.get_executor();
        asio::steady_timer timer(ex);
        timer.expires_after(*timeout);

        auto read_task = asio::co_spawn(
            ex,
            asio::async_read(port_,
                             asio::buffer(&b, 1),
                             asio::as_tuple(asio::use_awaitable)),
            asio::deferred);

        auto timer_task =
            asio::co_spawn(ex,
                           timer.async_wait(asio::as_tuple(asio::use_awaitable)),
                           asio::deferred);

        auto [order, read_ex, read_result, timer_ex, timer_result] =
            co_await asio::experimental::make_parallel_group(
                std::move(read_task), std::move(timer_task))
                .async_wait(asio::experimental::wait_for_one(),
                            asio::as_tuple(asio::use_awaitable));

        (void)timer_result;

        if (read_ex || timer_ex) {
            co_return std::pair{make_error_code(errc::invalid_argument),
                                secs::core::byte{0}};
        }

        if (order[0] == 0) {
            auto [ec, n] = read_result;
            if (ec) {
                co_return std::pair{ec, secs::core::byte{0}};
            }
            if (n != 1) {
                co_return std::pair{make_error_code(errc::invalid_argument),
                                    secs::core::byte{0}};
            }
            co_return std::pair{std::error_code{}, b};
        }

        // timer 先到：按超时处理；同时 cancel serial_port 以加速底层 read 退出。
        std::error_code ignored;
        port_.cancel(ignored);
        co_return std::pair{make_error_code(errc::timeout), secs::core::byte{0}};
    }

private:
    asio::any_io_executor ex_{};
    asio::serial_port port_;
};

} // namespace secs::secs1

