#pragma once

#include "secs/core/common.hpp"
#include "secs/secs1/block.hpp"
#include "secs/secs1/link.hpp"
#include "secs/secs1/timer.hpp"

#include <asio/awaitable.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <system_error>
#include <utility>
#include <vector>

namespace secs::secs1 {

enum class State : std::uint8_t {
  idle = 0,
  wait_eot = 1,
  wait_block = 2,
  wait_check = 3,
};

struct ReceivedMessage final {
  Header header{};
  std::vector<secs::core::byte> body{};
};

/**
 * @brief SECS-I 传输层状态机（协程化）。
 *
 * 覆盖能力：
 * - ENQ/EOT 握手（支持 NAK 拒绝 + 重试）
 * - Block 分包/重组（≤243B/block）
 * - Checksum/DeviceID 校验（失败返回 NAK）
 * - T1/T2/T3/T4 超时
 */
class StateMachine final {
 public:
  explicit StateMachine(
    Link& link,
    std::optional<std::uint16_t> expected_device_id = std::nullopt,
    Timeouts timeouts = {},
    std::size_t retry_limit = 3);

  [[nodiscard]] asio::any_io_executor executor() const noexcept { return link_.executor(); }

  [[nodiscard]] State state() const noexcept { return state_; }
  [[nodiscard]] const Timeouts& timeouts() const noexcept { return timeouts_; }

  asio::awaitable<std::error_code> async_send(const Header& header, secs::core::bytes_view body);

  asio::awaitable<std::pair<std::error_code, ReceivedMessage>> async_receive(
    std::optional<secs::core::duration> timeout = std::nullopt);

  asio::awaitable<std::pair<std::error_code, ReceivedMessage>> async_transact(
    const Header& header,
    secs::core::bytes_view body);

 private:
  asio::awaitable<std::error_code> async_send_control(secs::core::byte b);

  asio::awaitable<std::pair<std::error_code, secs::core::byte>> async_read_byte(
    std::optional<secs::core::duration> timeout);

  Link& link_;
  std::optional<std::uint16_t> expected_device_id_{};
  Timeouts timeouts_{};
  std::size_t retry_limit_{3};
  State state_{State::idle};
};

}  // namespace secs::secs1
