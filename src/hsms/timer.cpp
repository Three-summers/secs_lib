#include "secs/hsms/timer.hpp"

#include <asio/error.hpp>

namespace secs::hsms {

Timer::Timer(asio::any_io_executor ex) : timer_(ex) {}

void Timer::cancel() noexcept {
  timer_.cancel();
}

asio::awaitable<std::error_code> Timer::async_wait_for(core::duration d) {
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

}  // namespace secs::hsms
