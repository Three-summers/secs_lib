#include "secs/secs1/timer.hpp"

#include "secs/core/error.hpp"

#include <asio/as_tuple.hpp>
#include <asio/error.hpp>
#include <asio/use_awaitable.hpp>

namespace secs::secs1 {

/*
 * SECS-I 传输层使用的协程定时器封装：
 *
 * - 目的：把 asio::steady_timer 的等待结果统一映射为 std::error_code，
 *   便于在 SECS-I 状态机中“以返回值控制流程”，而不是依赖异常或回调。
 * - steady_timer 基于单调时钟，不受系统时间跳变影响，适合实现 T1/T2/T3/T4 等协议超时。
 */

Timer::Timer(asio::any_io_executor ex) : timer_(ex) {}

void Timer::cancel() noexcept {
    try {
        timer_.cancel();
    } catch (...) {
        // cancel() 可能抛出 asio::system_error；在 noexcept 路径中吞掉，
        // 避免异常触发 std::terminate。
    }
}

asio::awaitable<std::error_code> Timer::async_sleep(secs::core::duration d) {
    // 与 hsms::Timer 一致：把 asio::steady_timer 的结果映射到 std::error_code。
    timer_.expires_after(d);
    auto [ec] = co_await timer_.async_wait(asio::as_tuple(asio::use_awaitable));
    if (!ec) {
        co_return std::error_code{};
    }
    if (ec == asio::error::operation_aborted) {
        co_return secs::core::make_error_code(secs::core::errc::cancelled);
    }
    co_return ec;
}

} // namespace secs::secs1
