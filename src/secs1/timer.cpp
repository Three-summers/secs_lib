#include "secs/secs1/timer.hpp"

#include "secs/core/error.hpp"

#include <asio/as_tuple.hpp>
#include <asio/error.hpp>
#include <asio/use_awaitable.hpp>

namespace secs::secs1 {

Timer::Timer(asio::any_io_executor ex) : timer_(ex) {}

void Timer::cancel() noexcept {
  timer_.cancel();
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

}  // namespace secs::secs1
