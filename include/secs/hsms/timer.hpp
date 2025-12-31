#pragma once

#include "secs/core/common.hpp"
#include "secs/core/error.hpp"

#include <asio/as_tuple.hpp>
#include <asio/awaitable.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>

#include <system_error>

namespace secs::hsms {

/**
 * @brief HSMS 定时器封装（基于 asio::steady_timer，返回 std::error_code）。
 *
 * 约定：
 * - async_wait_for(d)：等待 d 时长到期，正常到期返回 ok
 * - cancel()：取消等待，等待者返回 cancelled
 */
class Timer final {
 public:
  explicit Timer(asio::any_io_executor ex);

  void cancel() noexcept;

  asio::awaitable<std::error_code> async_wait_for(core::duration d);

 private:
  asio::steady_timer timer_;
};

}  // 命名空间 secs::hsms
