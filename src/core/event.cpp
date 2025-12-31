#include "secs/core/event.hpp"

#include <asio/error.hpp>

namespace secs::core {

/*
 * Event 的实现要点（理解 set/reset/cancel/timeout 的交互）：
 *
 * - 等待者通过 asio::steady_timer 挂起；set()/cancel() 会对所有 waiter 执行 timer->cancel() 来“唤醒”。
 *
 * - 由于 timer->cancel() 与 timer 到期都会让 async_wait 返回，因此需要用两个 generation 计数区分来源：
 *   - set_generation_ 变化：说明被 set() 唤醒 -> 返回成功
 *   - cancel_generation_ 变化：说明被 cancel() 唤醒 -> 返回 cancelled
 *   - 两者都没变化：说明是“超时到期”或“底层错误”
 */
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

  // 记录当前 generation，用于在等待结束后判断“是 set/cancel 导致的唤醒”还是“超时/错误”。
  const auto local_set_gen = set_generation_;
  const auto local_cancel_gen = cancel_generation_;

  auto ex = co_await asio::this_coro::executor;
  auto timer = std::make_shared<asio::steady_timer>(ex);

  if (timeout.has_value()) {
    timer->expires_after(*timeout);
  } else {
    // 没有超时时间时，用一个“很远的时间点”模拟永久等待。
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
    // 定时器正常到期（没有错误码）-> errc::timeout
    co_return make_error_code(errc::timeout);
  }

  if (ec == asio::error::operation_aborted) {
    // 定时器被 cancel() 取消（set/cancel 都会 cancel 定时器）-> errc::cancelled
    co_return make_error_code(errc::cancelled);
  }

  co_return ec;
}

}  // namespace secs::core
