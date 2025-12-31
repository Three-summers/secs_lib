#include "secs/core/event.hpp"

#include <asio/error.hpp>

namespace secs::core {

void Event::cancel_waiters_() noexcept {
  for (const auto& timer : waiters_) {
    timer->cancel();
  }
}

void Event::set() noexcept {
  signaled_ = true;
  ++set_generation_;
  cancel_waiters_();
}

void Event::reset() noexcept {
  signaled_ = false;
}

void Event::cancel() noexcept {
  ++cancel_generation_;
  cancel_waiters_();
}

asio::awaitable<std::error_code> Event::async_wait(
  std::optional<steady_clock::duration> timeout) {
  if (signaled_) {
    co_return std::error_code{};
  }

  const auto local_set_gen = set_generation_;
  const auto local_cancel_gen = cancel_generation_;

  auto ex = co_await asio::this_coro::executor;
  auto timer = std::make_shared<asio::steady_timer>(ex);

  if (timeout.has_value()) {
    timer->expires_after(*timeout);
  } else {
    timer->expires_at(asio::steady_timer::time_point::max());
  }

  auto it = waiters_.insert(waiters_.end(), timer);
  auto [ec] = co_await timer->async_wait(asio::as_tuple(asio::use_awaitable));
  waiters_.erase(it);

  if (set_generation_ != local_set_gen) {
    co_return std::error_code{};
  }
  if (cancel_generation_ != local_cancel_gen) {
    co_return make_error_code(errc::cancelled);
  }

  if (!ec) {
    co_return make_error_code(errc::timeout);
  }

  if (ec == asio::error::operation_aborted) {
    co_return make_error_code(errc::cancelled);
  }

  co_return ec;
}

}  // namespace secs::core
