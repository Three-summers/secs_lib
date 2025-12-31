#pragma once

#include "secs/core/common.hpp"

#include <asio/any_io_executor.hpp>
#include <asio/awaitable.hpp>
#include <asio/steady_timer.hpp>

#include <chrono>
#include <optional>
#include <system_error>

namespace secs::secs1 {

struct Timeouts final {
  secs::core::duration t1_intercharacter{std::chrono::seconds(1)};
  secs::core::duration t2_protocol{std::chrono::seconds(3)};
  secs::core::duration t3_reply{std::chrono::seconds(45)};
  secs::core::duration t4_interblock{std::chrono::seconds(45)};
};

/**
 * @brief 基于 asio::steady_timer 的轻量定时器封装。
 *
 * 设计目标：
 * - 统一返回 std::error_code（避免异常路径）
 * - 可 cancel（用于测试/收尾）
 */
class Timer final {
 public:
  explicit Timer(asio::any_io_executor ex);

  void cancel() noexcept;

  asio::awaitable<std::error_code> async_sleep(secs::core::duration d);

 private:
  asio::steady_timer timer_;
};

}  // namespace secs::secs1
