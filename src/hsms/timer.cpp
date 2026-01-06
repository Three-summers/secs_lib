#include "secs/hsms/timer.hpp"

#include <asio/error.hpp>

namespace secs::hsms {

/*
 * HSMS 定时器封装实现。
 *
 * 目的：
 * - 统一把 asio::steady_timer 的等待结果映射为 std::error_code，保持库内“无异常”
 *   的控制流风格；
 * - 供 HSMS Session/Connection 用于实现 T5/T6/T7/T8 等等待与退避逻辑。
 *
 * 约定（与 include/secs/hsms/timer.hpp 保持一致）：
 * - 正常到期：返回空 error_code
 * - cancel() 触发的 operation_aborted：映射为 secs::core::errc::cancelled
 * - 其他底层错误：直接透传
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

asio::awaitable<std::error_code> Timer::async_wait_for(core::duration d) {
    // 统一把 asio::steady_timer 的结果映射到 std::error_code：
    // - 正常到期：成功（返回空 error_code）
    // - operation_aborted：映射为 errc::cancelled
    // - 其他：透传底层错误
    timer_.expires_after(d);
    auto [ec] = co_await timer_.async_wait(asio::as_tuple(asio::use_awaitable));
    if (!ec) {
        co_return std::error_code{};
    }
    if (ec == asio::error::operation_aborted) {
        co_return core::make_error_code(core::errc::cancelled);
    }
    co_return ec;
}

} // namespace secs::hsms
