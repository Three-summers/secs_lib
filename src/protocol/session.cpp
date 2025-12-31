#include "secs/protocol/session.hpp"

#include "secs/core/error.hpp"
#include "secs/hsms/message.hpp"
#include "secs/hsms/session.hpp"
#include "secs/secs1/block.hpp"
#include "secs/secs1/state_machine.hpp"

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>

#include <chrono>

namespace secs::protocol {
namespace {

using secs::core::errc;
using secs::core::make_error_code;

[[nodiscard]] bool is_valid_stream(std::uint8_t stream) noexcept {
  return stream <= 0x7FU;
}

[[nodiscard]] bool is_primary_function(std::uint8_t function) noexcept {
  return function != 0U && (function & 0x01U) != 0U;
}

[[nodiscard]] bool can_compute_secondary_function(std::uint8_t primary_function) noexcept {
  return primary_function != 0xFFU;
}

[[nodiscard]] std::uint8_t secondary_function(std::uint8_t primary_function) noexcept {
  return static_cast<std::uint8_t>(primary_function + 1U);
}

[[nodiscard]] std::optional<secs::core::duration> normalize_timeout(secs::core::duration d) noexcept {
  if (d == secs::core::duration{}) {
    return std::nullopt;
  }
  return d;
}

}  // namespace

Session::Session(secs::hsms::Session& hsms, std::uint16_t session_id, SessionOptions options)
  : backend_(Backend::hsms),
    executor_(hsms.executor()),
    options_(options),
    hsms_(&hsms),
    hsms_session_id_(session_id) {}

Session::Session(secs::secs1::StateMachine& secs1, std::uint16_t device_id, SessionOptions options)
  : backend_(Backend::secs1),
    executor_(secs1.executor()),
    options_(options),
    secs1_(&secs1),
    secs1_device_id_(device_id) {}

bool Session::is_running_() const noexcept {
  return run_loop_active_;
}

void Session::ensure_hsms_run_loop_started_() {
  std::lock_guard lk(run_mu_);
  if (run_loop_spawned_) {
    return;
  }
  run_loop_spawned_ = true;

  asio::co_spawn(
    executor_,
    [this]() -> asio::awaitable<void> { co_await async_run(); },  // GCOVR_EXCL_LINE：co_spawn 内联分支不计入覆盖率
    asio::detached);
}

void Session::stop() noexcept {
  stop_requested_ = true;
  cancel_all_pending_(make_error_code(errc::cancelled));
}

asio::awaitable<void> Session::async_run() {
  if (run_loop_active_) {
    co_return;
  }

  run_loop_active_ = true;
  struct Reset final {
    Session* self;
    ~Reset() { self->run_loop_active_ = false; }
  } reset{this};

  const auto timeout = normalize_timeout(options_.poll_interval);

  while (!stop_requested_) {
    auto [ec, msg] = co_await async_receive_message_(timeout);
    if (ec == make_error_code(errc::timeout)) {
      continue;
    }
    if (ec) {
      cancel_all_pending_(ec);
      stop_requested_ = true;
      break;
    }
    co_await handle_inbound_(std::move(msg));
  }
}

asio::awaitable<std::error_code> Session::async_send(
  std::uint8_t stream,
  std::uint8_t function,
  secs::core::bytes_view body) {
  if (!is_valid_stream(stream) || !is_primary_function(function)) {
    co_return make_error_code(errc::invalid_argument);
  }

  std::uint32_t sb = 0;
  auto alloc_ec = system_bytes_.allocate(sb);
  if (alloc_ec) {
    co_return alloc_ec;
  }

  DataMessage msg{};
  msg.stream = stream;
  msg.function = function;
  msg.w_bit = false;
  msg.system_bytes = sb;
  msg.body.assign(body.begin(), body.end());

  auto ec = co_await async_send_message_(msg);
  system_bytes_.release(sb);
  co_return ec;
}

asio::awaitable<std::pair<std::error_code, DataMessage>> Session::async_request(
  std::uint8_t stream,
  std::uint8_t function,
  secs::core::bytes_view body,
  std::optional<secs::core::duration> timeout) {
  if (!is_valid_stream(stream) || !is_primary_function(function) || !can_compute_secondary_function(function)) {
    co_return std::pair{make_error_code(errc::invalid_argument), DataMessage{}};
  }

  const auto expected_function = secondary_function(function);
  const auto t3 = timeout.value_or(options_.t3);

  std::uint32_t sb = 0;
  auto alloc_ec = system_bytes_.allocate(sb);
  if (alloc_ec) {
    co_return std::pair{alloc_ec, DataMessage{}};
  }

  DataMessage req{};
  req.stream = stream;
  req.function = function;
  req.w_bit = true;
  req.system_bytes = sb;
  req.body.assign(body.begin(), body.end());

  // HSMS：用 run loop 统一接收并分发，避免多个 request 并发读造成竞争。
  if (backend_ == Backend::hsms) {
    ensure_hsms_run_loop_started_();

    auto pending = std::make_shared<Pending>(stream, expected_function);
    {
      std::lock_guard lk(pending_mu_);
      pending_.insert_or_assign(sb, pending);
    }

    auto send_ec = co_await async_send_message_(req);
    if (send_ec) {
      {
        std::lock_guard lk(pending_mu_);
        pending_.erase(sb);
      }
      system_bytes_.release(sb);
      co_return std::pair{send_ec, DataMessage{}};
    }

    auto wait_ec = co_await pending->ready.async_wait(t3);
    {
      std::lock_guard lk(pending_mu_);
      pending_.erase(sb);
    }
    system_bytes_.release(sb);

    if (wait_ec == make_error_code(errc::timeout)) {
      co_return std::pair{wait_ec, DataMessage{}};
    }
    if (wait_ec) {
      co_return std::pair{pending->ec ? pending->ec : wait_ec, DataMessage{}};
    }
    if (pending->ec) {
      co_return std::pair{pending->ec, DataMessage{}};
    }
    if (!pending->response.has_value()) {
      co_return std::pair{make_error_code(errc::invalid_argument), DataMessage{}};
    }
    co_return std::pair{std::error_code{}, *pending->response};
  }

  // SECS-I：半双工，request 自己驱动接收循环并在期间处理可能的入站 primary。
  auto send_ec = co_await async_send_message_(req);
  if (send_ec) {
    system_bytes_.release(sb);
    co_return std::pair{send_ec, DataMessage{}};
  }

  const auto deadline = secs::core::steady_clock::now() + t3;
  for (;;) {
    const auto now = secs::core::steady_clock::now();
    if (now >= deadline) {
      system_bytes_.release(sb);
      co_return std::pair{make_error_code(errc::timeout), DataMessage{}};
    }

    const auto remaining = deadline - now;
    auto [ec, msg] = co_await async_receive_message_(remaining);
    if (ec) {
      system_bytes_.release(sb);
      co_return std::pair{ec, DataMessage{}};
    }

    const bool matches =
      msg.is_secondary() &&
      !msg.w_bit &&
      msg.system_bytes == sb &&
      msg.stream == stream &&
      msg.function == expected_function;

    if (matches) {
      system_bytes_.release(sb);
      co_return std::pair{std::error_code{}, std::move(msg)};
    }

    co_await handle_inbound_(std::move(msg));
  }
}

asio::awaitable<std::error_code> Session::async_send_message_(const DataMessage& msg) {
  if (stop_requested_) {
    co_return make_error_code(errc::cancelled);
  }

  if (backend_ == Backend::hsms) {
    if (!hsms_) {
      co_return make_error_code(errc::invalid_argument);
    }
    const auto wire = secs::hsms::make_data_message(
      hsms_session_id_,
      msg.stream,
      msg.function,
      msg.w_bit,
      msg.system_bytes,
      secs::core::bytes_view{msg.body.data(), msg.body.size()});
    co_return co_await hsms_->async_send(wire);
  }

  if (!secs1_) {
    co_return make_error_code(errc::invalid_argument);
  }

  secs::secs1::Header h{};
  h.reverse_bit = false;
  h.device_id = secs1_device_id_;
  h.wait_bit = msg.w_bit;
  h.stream = msg.stream;
  h.function = msg.function;
  h.end_bit = true;
  h.block_number = 1;
  h.system_bytes = msg.system_bytes;

  co_return co_await secs1_->async_send(h, secs::core::bytes_view{msg.body.data(), msg.body.size()});
}

asio::awaitable<std::pair<std::error_code, DataMessage>> Session::async_receive_message_(
  std::optional<secs::core::duration> timeout) {
  if (stop_requested_) {
    co_return std::pair{make_error_code(errc::cancelled), DataMessage{}};
  }

  if (backend_ == Backend::hsms) {
    if (!hsms_) {
      co_return std::pair{make_error_code(errc::invalid_argument), DataMessage{}};
    }

    auto [ec, msg] = co_await hsms_->async_receive_data(timeout);
    if (ec) {
      co_return std::pair{ec, DataMessage{}};
    }

    DataMessage out{};
    out.stream = msg.stream();
    out.function = msg.function();
    out.w_bit = msg.w_bit();
    out.system_bytes = msg.header.system_bytes;
    out.body = std::move(msg.body);
    co_return std::pair{std::error_code{}, std::move(out)};
  }

  if (!secs1_) {
    co_return std::pair{make_error_code(errc::invalid_argument), DataMessage{}};
  }

  auto [ec, msg] = co_await secs1_->async_receive(timeout);
  if (ec) {
    co_return std::pair{ec, DataMessage{}};
  }

  DataMessage out{};
  out.stream = msg.header.stream;
  out.function = msg.header.function;
  out.w_bit = msg.header.wait_bit;
  out.system_bytes = msg.header.system_bytes;
  out.body = std::move(msg.body);
  co_return std::pair{std::error_code{}, std::move(out)};
}

asio::awaitable<void> Session::handle_inbound_(DataMessage msg) {
  if (try_fulfill_pending_(msg)) {
    co_return;
  }

  // 未匹配的 secondary：忽略（可能是迟到回应/对端异常发送）。
  if (msg.is_secondary()) {
    co_return;
  }

  auto handler_opt = router_.find(msg.stream, msg.function);
  if (!handler_opt.has_value()) {
    co_return;
  }

  auto handler = std::move(*handler_opt);
  auto [ec, rsp_body] = co_await handler(msg);
  if (ec) {
    co_return;
  }

  if (!msg.w_bit) {
    co_return;
  }
  if (!can_compute_secondary_function(msg.function)) {
    co_return;
  }

  DataMessage rsp{};
  rsp.stream = msg.stream;
  rsp.function = secondary_function(msg.function);
  rsp.w_bit = false;
  rsp.system_bytes = msg.system_bytes;
  rsp.body = std::move(rsp_body);
  (void)co_await async_send_message_(rsp);
}

bool Session::try_fulfill_pending_(DataMessage& msg) noexcept {
  std::shared_ptr<Pending> pending;
  {
    std::lock_guard lk(pending_mu_);
    const auto it = pending_.find(msg.system_bytes);
    if (it == pending_.end()) {
      return false;
    }
    pending = it->second;
  }

  // pending_ 的写入只来自 async_request（make_shared），因此这里不做空指针分支。
  if (!msg.is_secondary() || msg.w_bit) {
    return false;
  }
  if (pending->expected_stream != msg.stream || pending->expected_function != msg.function) {
    return false;
  }

  pending->response = std::move(msg);
  pending->ec = std::error_code{};
  pending->ready.set();
  return true;
}

void Session::cancel_all_pending_(std::error_code reason) noexcept {
  std::unordered_map<std::uint32_t, std::shared_ptr<Pending>> moved;
  {
    std::lock_guard lk(pending_mu_);
    moved.swap(pending_);
  }

  for (auto& [sb, pending] : moved) {
    pending->ec = reason;
    pending->ready.cancel();
    system_bytes_.release(sb);
  }
}

}  // namespace secs::protocol
