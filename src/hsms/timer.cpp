#include "secs/hsms/timer.hpp"

#include <asio/error.hpp>

namespace secs::hsms {

Timer::Timer(asio::any_io_executor ex) : timer_(ex) {}

void Timer::cancel() noexcept {
  timer_.cancel();
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

}  // 命名空间 secs::hsms
