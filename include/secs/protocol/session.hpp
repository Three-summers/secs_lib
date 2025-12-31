#pragma once

#include "secs/core/common.hpp"
#include "secs/core/event.hpp"
#include "secs/protocol/router.hpp"
#include "secs/protocol/system_bytes.hpp"

#include <asio/any_io_executor.hpp>
#include <asio/awaitable.hpp>

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <system_error>
#include <unordered_map>
#include <utility>

namespace secs::hsms {
class Session;
}  // namespace secs::hsms

namespace secs::secs1 {
class StateMachine;
}  // namespace secs::secs1

namespace secs::protocol {

struct SessionOptions final {
  // T3：回复超时（协议层请求-响应匹配用）。
  secs::core::duration t3{std::chrono::seconds{45}};

  // 接收循环的轮询间隔：用于 stop() 检查与避免永久阻塞。
  secs::core::duration poll_interval{std::chrono::milliseconds{10}};
};

/**
 * @brief 协议层会话：统一 HSMS 与 SECS-I 的“发送/请求/接收循环”接口。
 *
 * 能力：
 * - SystemBytes 分配与追踪（释放重用、回绕）
 * - 基于 SystemBytes + (Stream/Function) 的请求-响应匹配（T3）
 * - 根据 (Stream, Function) 路由入站主消息到注册的处理器，并在 W 位=1 时自动回从消息
 *
 * 说明：
 * - HSMS（全双工）推荐同时运行 async_run，用于接收并分发消息、唤醒挂起的请求。
 * - SECS-I（半双工）不建议在 async_run 运行期间并发调用 async_request/async_send。
 */
class Session final {
 public:
  Session(secs::hsms::Session& hsms, std::uint16_t session_id, SessionOptions options = {});
  Session(secs::secs1::StateMachine& secs1, std::uint16_t device_id, SessionOptions options = {});

  [[nodiscard]] asio::any_io_executor executor() const noexcept { return executor_; }

  [[nodiscard]] Router& router() noexcept { return router_; }
  [[nodiscard]] const Router& router() const noexcept { return router_; }

  void stop() noexcept;

  // 接收循环：持续接收入站消息并处理（匹配挂起请求 / 路由处理器）。
  asio::awaitable<void> async_run();

  // 发送主消息（W=0，不等待回应）。
  asio::awaitable<std::error_code> async_send(
    std::uint8_t stream,
    std::uint8_t function,
    secs::core::bytes_view body);

  // 发送主消息（W=1）并等待从消息（T3 超时）。
  asio::awaitable<std::pair<std::error_code, DataMessage>> async_request(
    std::uint8_t stream,
    std::uint8_t function,
    secs::core::bytes_view body,
    std::optional<secs::core::duration> timeout = std::nullopt);

 private:
  enum class Backend : std::uint8_t {
    hsms = 0,
    secs1 = 1,
  };

  struct Pending final {
    Pending(std::uint8_t stream, std::uint8_t function) : expected_stream(stream), expected_function(function) {}

    std::uint8_t expected_stream{0};
    std::uint8_t expected_function{0};
    secs::core::Event ready{};
    std::error_code ec{};
    std::optional<DataMessage> response{};
  };

  [[nodiscard]] bool is_running_() const noexcept;

  asio::awaitable<std::error_code> async_send_message_(const DataMessage& msg);
  asio::awaitable<std::pair<std::error_code, DataMessage>> async_receive_message_(
    std::optional<secs::core::duration> timeout);

  asio::awaitable<void> handle_inbound_(DataMessage msg);
  [[nodiscard]] bool try_fulfill_pending_(DataMessage& msg) noexcept;
  void cancel_all_pending_(std::error_code reason) noexcept;

  void ensure_hsms_run_loop_started_();

  Backend backend_{Backend::hsms};
  asio::any_io_executor executor_{};
  SessionOptions options_{};

  SystemBytes system_bytes_{};
  Router router_{};

  mutable std::mutex pending_mu_{};
  std::unordered_map<std::uint32_t, std::shared_ptr<Pending>> pending_{};

  bool stop_requested_{false};
  bool run_loop_active_{false};
  bool run_loop_spawned_{false};
  std::mutex run_mu_{};

  secs::hsms::Session* hsms_{nullptr};
  std::uint16_t hsms_session_id_{0};

  secs::secs1::StateMachine* secs1_{nullptr};
  std::uint16_t secs1_device_id_{0};
};

}  // namespace secs::protocol
