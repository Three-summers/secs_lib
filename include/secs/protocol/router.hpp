#pragma once

#include "secs/core/common.hpp"

#include <asio/awaitable.hpp>

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace secs::protocol {

/**
 * @brief 协议层数据消息（抽象自 HSMS data message 与 SECS-I message）。
 */
struct DataMessage final {
  std::uint8_t stream{0};
  std::uint8_t function{0};
  bool w_bit{false};
  std::uint32_t system_bytes{0};
  std::vector<secs::core::byte> body{};

  [[nodiscard]] bool is_primary() const noexcept { return (function & 0x01U) != 0; }
  [[nodiscard]] bool is_secondary() const noexcept { return !is_primary(); }
};

using HandlerResult = std::pair<std::error_code, std::vector<secs::core::byte>>;
using Handler = std::function<asio::awaitable<HandlerResult>(const DataMessage&)>;

/**
 * @brief 基于 Stream/Function 的消息路由器。
 *
 * 说明：
 * - key 采用 (stream,function) 二元组；不支持通配符，保持实现简单可控。
 * - handler 为协程函数，返回 response body；错误通过 std::error_code 返回。
 */
class Router final {
 public:
  Router() = default;

  void set(std::uint8_t stream, std::uint8_t function, Handler handler);
  void erase(std::uint8_t stream, std::uint8_t function) noexcept;
  void clear() noexcept;

  [[nodiscard]] std::optional<Handler> find(std::uint8_t stream, std::uint8_t function) const;

 private:
  using Key = std::uint16_t;

  static constexpr Key make_key_(std::uint8_t stream, std::uint8_t function) noexcept {
    return static_cast<Key>(
      (static_cast<Key>(stream) << 8U) | static_cast<Key>(function));
  }

  mutable std::mutex mu_{};
  std::unordered_map<Key, Handler> handlers_{};
};

}  // namespace secs::protocol

