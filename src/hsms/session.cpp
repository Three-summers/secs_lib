#include "secs/hsms/session.hpp"

#include "secs/core/error.hpp"
#include "secs/hsms/timer.hpp"

#include <asio/as_tuple.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

namespace secs::hsms {
namespace {

constexpr std::uint16_t kRspOk = 0;
constexpr std::uint16_t kRspReject = 1;

}  // namespace

/*
 * HSMS::Session 的协程并发模型（便于理解“为什么要有 pending_/Event/reader_loop_”）：
 *
 * 1) 读方向：start_reader_() 启动一个常驻 reader_loop_ 协程，串行读取 connection_ 的 HSMS 帧。
 *
 * 2) 控制事务/数据事务：
 *    - async_control_transaction_ / async_data_transaction_ 在发送请求前，把 (system_bytes -> Pending) 登记到 pending_。
 *    - reader_loop_ 收到响应后，通过 fulfill_pending_() 唤醒对应 Pending::ready。
 *
 * 3) 数据消息队列：
 *    - 未被 pending_ 消费的 data message 会进入 inbound_data_；
 *    - async_receive_data() 只负责从 inbound_data_ 取“下一条 data”交给上层。
 *
 * 4) 断线处理：
 *    - on_disconnected_ 统一取消 selected_event_/inbound_event_ 以及所有 pending，
 *      避免协程永久挂起。
 *
 * 5) selected_generation_：
 *    - 每次进入 selected 都会递增 generation，用于让旧连接周期的 linktest_loop_ 在重连后自动退出。
 */
Session::Session(asio::any_io_executor ex, SessionOptions options)
  : executor_(ex),
    options_(options),
    connection_(ex, ConnectionOptions{.t8 = options.t8}) {}

void Session::reset_state_() noexcept {
  state_ = SessionState::connected;

  selected_event_.reset();
  inbound_event_.reset();
  disconnected_event_.reset();

  inbound_data_.clear();
  pending_.clear();
}

void Session::set_selected_() noexcept {
  if (state_ == SessionState::selected) {
    return;
  }
  state_ = SessionState::selected;
  const auto gen = selected_generation_.fetch_add(1U) + 1U;
  selected_event_.set();

  if (options_.linktest_interval != core::duration{}) {
    asio::co_spawn(
      executor_,
      [this, gen]() -> asio::awaitable<void> { co_await linktest_loop_(gen); },  // GCOVR_EXCL_LINE：co_spawn 内联分支不计入覆盖率
      asio::detached);
  }
}

void Session::on_disconnected_(std::error_code reason) noexcept {
  state_ = SessionState::disconnected;

  // 唤醒所有等待者：已选择（selected）状态等待、入站队列等待、挂起事务等待。
  selected_event_.cancel();
  selected_event_.reset();
  inbound_event_.cancel();
  inbound_event_.reset();
  reader_running_ = false;
  disconnected_event_.set();

  for (auto& [_, pending] : pending_) {
    pending->ec = reason;
    pending->ready.cancel();
  }
  pending_.clear();
  inbound_data_.clear();
}

void Session::stop() noexcept {
  stop_requested_ = true;
  connection_.cancel_and_close();

  on_disconnected_(core::make_error_code(core::errc::cancelled));
}

void Session::start_reader_() {
  reader_running_ = true;
  disconnected_event_.reset();

  asio::co_spawn(
    executor_,
    [this]() -> asio::awaitable<void> { co_await reader_loop_(); },  // GCOVR_EXCL_LINE：co_spawn 内联分支不计入覆盖率
    asio::detached);
}

asio::awaitable<void> Session::reader_loop_() {
  while (!stop_requested_) {
    auto [ec, msg] = co_await connection_.async_read_message();
    if (ec) {
      on_disconnected_(ec);
      break;
    }

    if (msg.is_data()) {
      if (fulfill_pending_(msg)) {
        continue;
      }
      inbound_data_.push_back(std::move(msg));
      inbound_event_.set();
      continue;
    }

    // 控制消息：SELECT/DESELECT/LINKTEST/SEPARATE 等。
    bool should_exit = false;
    switch (msg.header.s_type) {
      case SType::select_req: {
        // 被动端收到 SELECT.req：
        // - 可按配置拒绝（用于单元测试覆盖）
        // - 校验 SessionID（不匹配则拒绝并断线）
        // - 已处于 selected 时重复收到 SELECT.req：回拒绝，但保持连接
        if (!options_.passive_accept_select) {
          (void)co_await connection_.async_write_message(
            make_select_rsp(kRspReject, msg.header.system_bytes));
          (void)co_await connection_.async_close();
          on_disconnected_(core::make_error_code(core::errc::invalid_argument));
          should_exit = true;
          break;
        }

        if (state_ == SessionState::selected) {
          (void)co_await connection_.async_write_message(
            make_select_rsp(kRspReject, msg.header.system_bytes));
          break;
        }

        if (msg.header.session_id != options_.session_id) {
          (void)co_await connection_.async_write_message(
            make_select_rsp(kRspReject, msg.header.system_bytes));
          (void)co_await connection_.async_close();
          on_disconnected_(core::make_error_code(core::errc::invalid_argument));
          should_exit = true;
          break;
        }

        (void)co_await connection_.async_write_message(
          make_select_rsp(kRspOk, msg.header.system_bytes));
        set_selected_();
        break;
      }
      case SType::select_rsp: {
        // 主动端收到 SELECT.rsp：
        // - 先尝试唤醒挂起的控制事务（对应 async_control_transaction_）
        // - 若接受则进入 selected
        (void)fulfill_pending_(msg);
        if (msg.header.session_id == kRspOk) {
          set_selected_();
        }
        break;
      }
      case SType::deselect_req: {
        // 收到 DESELECT.req：按协议回复后断线（保持实现简单）
        (void)co_await connection_.async_write_message(
          make_deselect_rsp(kRspOk, msg.header.system_bytes));
        (void)co_await connection_.async_close();
        on_disconnected_(core::make_error_code(core::errc::cancelled));
        should_exit = true;
        break;
      }
      case SType::deselect_rsp: {
        (void)fulfill_pending_(msg);
        // DESELECT 交互完成后主动断开，保持实现简单。
        (void)co_await connection_.async_close();
        on_disconnected_(core::make_error_code(core::errc::cancelled));
        should_exit = true;
        break;
      }
      case SType::linktest_req: {
        // 收到 LINKTEST.req：立即回复 LINKTEST.rsp（不进入 inbound_data_）
        (void)co_await connection_.async_write_message(
          make_linktest_rsp(kRspOk, msg.header.system_bytes));
        break;
      }
      case SType::linktest_rsp: {
        (void)fulfill_pending_(msg);
        break;
      }
      case SType::separate_req: {
        (void)co_await connection_.async_close();
        on_disconnected_(core::make_error_code(core::errc::cancelled));
        should_exit = true;
        break;
      }
      default: {
        // 未实现的控制类型：忽略（后续可扩展 Reject 等）。
        break;
      }
    }

    if (should_exit) {
      break;
    }
  }

  reader_running_ = false;
}

asio::awaitable<void> Session::linktest_loop_(std::uint64_t generation) {
  Timer timer(executor_);

  while (!stop_requested_) {
    if (state_ != SessionState::selected || selected_generation_.load() != generation) {
      co_return;
    }

    auto ec = co_await timer.async_wait_for(options_.linktest_interval);
    if (ec) {
      co_return;
    }

    if (state_ != SessionState::selected || selected_generation_.load() != generation) {
      co_return;
    }

    ec = co_await async_linktest();
    if (ec) {
      (void)co_await connection_.async_close();
      co_return;
    }
  }
}

bool Session::fulfill_pending_(const Message& msg) noexcept {
  const auto it = pending_.find(msg.header.system_bytes);
  if (it == pending_.end()) {
    return false;
  }

  const auto& pending = it->second;

  if (pending->expected_stype != msg.header.s_type) {
    return false;
  }

  pending->response = msg;
  pending->ec = std::error_code{};
  pending->ready.set();
  return true;
}

asio::awaitable<std::pair<std::error_code, Message>> Session::async_control_transaction_(
  const Message& req,
  SType expected_rsp,
  core::duration timeout) {
  // 控制事务：把请求登记到 pending_，由 reader_loop_ 收到响应后唤醒。
  const auto sb = req.header.system_bytes;
  auto pending = std::make_shared<Pending>(expected_rsp);
  pending_.insert_or_assign(sb, pending);

  auto ec = co_await connection_.async_write_message(req);
  if (ec) {
    pending_.erase(sb);
    co_return std::pair{ec, Message{}};
  }

  ec = co_await pending->ready.async_wait(timeout);
  if (ec == core::make_error_code(core::errc::timeout)) {
    // 控制事务超时：按断线处理（关闭连接并取消所有等待者），避免会话进入“不一致的半状态”。
    pending_.erase(sb);
    (void)co_await connection_.async_close();
    on_disconnected_(ec);
    co_return std::pair{ec, Message{}};
  }

  pending_.erase(sb);
  if (ec) {
    co_return std::pair{pending->ec ? pending->ec : ec, Message{}};
  }
  if (pending->ec) {
    co_return std::pair{pending->ec, Message{}};
  }
  if (!pending->response.has_value()) {
    co_return std::pair{core::make_error_code(core::errc::invalid_argument), Message{}};
  }

  co_return std::pair{std::error_code{}, *pending->response};
}

asio::awaitable<std::pair<std::error_code, Message>> Session::async_data_transaction_(
  const Message& req,
  core::duration timeout) {
  // 数据事务（W=1）：同样用 pending_ 做请求-响应匹配，超时视为连接异常。
  const auto sb = req.header.system_bytes;
  auto pending = std::make_shared<Pending>(SType::data);
  pending_.insert_or_assign(sb, pending);

  auto ec = co_await connection_.async_write_message(req);
  if (ec) {
    pending_.erase(sb);
    co_return std::pair{ec, Message{}};
  }

  ec = co_await pending->ready.async_wait(timeout);
  if (ec == core::make_error_code(core::errc::timeout)) {
    pending_.erase(sb);
    (void)co_await connection_.async_close();
    on_disconnected_(ec);
    co_return std::pair{ec, Message{}};
  }

  pending_.erase(sb);
  if (ec) {
    co_return std::pair{pending->ec ? pending->ec : ec, Message{}};
  }
  if (pending->ec) {
    co_return std::pair{pending->ec, Message{}};
  }
  if (!pending->response.has_value()) {
    co_return std::pair{core::make_error_code(core::errc::invalid_argument), Message{}};
  }

  co_return std::pair{std::error_code{}, *pending->response};
}

asio::awaitable<std::error_code> Session::async_open_active(const asio::ip::tcp::endpoint& endpoint) {
  if (stop_requested_) {
    co_return core::make_error_code(core::errc::cancelled);
  }

  Connection conn(executor_, ConnectionOptions{.t8 = options_.t8});
  auto ec = co_await conn.async_connect(endpoint);
  if (ec) {
    on_disconnected_(ec);
    co_return ec;
  }

  co_return co_await async_open_active(std::move(conn));
}

asio::awaitable<std::error_code> Session::async_open_active(Connection&& connection) {
  if (stop_requested_) {
    co_return core::make_error_code(core::errc::cancelled);
  }

  if (reader_running_) {
    // 若旧连接的 reader_loop_ 仍在跑，先关闭旧连接并等待其退出，避免两个 reader 同时读不同连接造成状态混乱。
    (void)co_await connection_.async_close();
    (void)co_await disconnected_event_.async_wait(options_.t6);
  }

  connection_ = std::move(connection);
  reset_state_();

  start_reader_();

  Message req = make_select_req(options_.session_id, allocate_system_bytes());
  auto [tr_ec, rsp] =
    co_await async_control_transaction_(req, SType::select_rsp, options_.t6);
  if (tr_ec) {
    co_return tr_ec;
  }
  if (rsp.header.session_id != kRspOk) {
    (void)co_await connection_.async_close();
    on_disconnected_(core::make_error_code(core::errc::invalid_argument));
    co_return core::make_error_code(core::errc::invalid_argument);
  }

  set_selected_();
  co_return std::error_code{};
}

asio::awaitable<std::error_code> Session::async_open_passive(asio::ip::tcp::socket socket) {
  if (stop_requested_) {
    co_return core::make_error_code(core::errc::cancelled);
  }

  Connection conn(std::move(socket), ConnectionOptions{.t8 = options_.t8});
  co_return co_await async_open_passive(std::move(conn));
}

asio::awaitable<std::error_code> Session::async_open_passive(Connection&& connection) {
  if (stop_requested_) {
    co_return core::make_error_code(core::errc::cancelled);
  }

  if (reader_running_) {
    // 同主动端：确保不会有“两个 reader_loop_ 同时存在”。
    (void)co_await connection_.async_close();
    (void)co_await disconnected_event_.async_wait(options_.t6);
  }

  connection_ = std::move(connection);
  reset_state_();
  start_reader_();

  auto ec = co_await selected_event_.async_wait(options_.t7);
  if (ec) {
    (void)co_await connection_.async_close();
    on_disconnected_(ec);
    co_return ec;
  }

  co_return std::error_code{};
}

asio::awaitable<std::error_code> Session::async_run_active(const asio::ip::tcp::endpoint& endpoint) {
  while (!stop_requested_) {
    auto ec = co_await async_open_active(endpoint);
    if (ec) {
      if (!options_.auto_reconnect || stop_requested_) {
        co_return ec;
      }
      // 连接失败：按 T5 退避后重试。
      Timer timer(executor_);
      (void)co_await timer.async_wait_for(options_.t5);
      continue;
    }

    // 等待断线，然后按 T5 退避后重连。
    (void)co_await disconnected_event_.async_wait(std::nullopt);
    if (!options_.auto_reconnect || stop_requested_) {
      co_return std::error_code{};
    }

    Timer timer(executor_);
    (void)co_await timer.async_wait_for(options_.t5);
  }
  co_return std::error_code{};
}

asio::awaitable<std::error_code> Session::async_send(const Message& msg) {
  if (!connection_.is_open()) {
    co_return core::make_error_code(core::errc::invalid_argument);
  }
  if (msg.is_data() && state_ != SessionState::selected) {
    co_return core::make_error_code(core::errc::invalid_argument);
  }
  co_return co_await connection_.async_write_message(msg);
}

asio::awaitable<std::pair<std::error_code, Message>> Session::async_receive_data(
  std::optional<core::duration> timeout) {
  while (inbound_data_.empty()) {
    auto ec = co_await inbound_event_.async_wait(timeout);
    if (ec) {
      co_return std::pair{ec, Message{}};
    }
  }

  Message msg = std::move(inbound_data_.front());
  inbound_data_.pop_front();
  if (inbound_data_.empty()) {
    inbound_event_.reset();
  }
  co_return std::pair{std::error_code{}, std::move(msg)};
}

asio::awaitable<std::pair<std::error_code, Message>> Session::async_request_data(
  std::uint8_t stream,
  std::uint8_t function,
  core::bytes_view body) {
  if (state_ != SessionState::selected) {
    co_return std::pair{core::make_error_code(core::errc::invalid_argument), Message{}};
  }

  const auto sb = allocate_system_bytes();
  Message req = make_data_message(options_.session_id, stream, function, true, sb, body);
  co_return co_await async_data_transaction_(req, options_.t3);
}

asio::awaitable<std::error_code> Session::async_linktest() {
  if (state_ != SessionState::selected) {
    co_return core::make_error_code(core::errc::invalid_argument);
  }

  Message req = make_linktest_req(options_.session_id, allocate_system_bytes());
  auto [ec, rsp] = co_await async_control_transaction_(req, SType::linktest_rsp, options_.t6);
  if (ec) {
    co_return ec;
  }
  if (rsp.header.session_id != kRspOk) {
    co_return core::make_error_code(core::errc::invalid_argument);
  }
  co_return std::error_code{};
}

asio::awaitable<std::error_code> Session::async_wait_selected(
  std::uint64_t min_generation,
  core::duration timeout) {
  const auto deadline = core::steady_clock::now() + timeout;
  while (!stop_requested_) {
    if (state_ == SessionState::selected && selected_generation_.load() >= min_generation) {
      co_return std::error_code{};
    }

    const auto now = core::steady_clock::now();
    if (now >= deadline) {
      co_return core::make_error_code(core::errc::timeout);
    }

    const auto remaining = deadline - now;
    auto ec = co_await selected_event_.async_wait(remaining);
    if (ec == core::make_error_code(core::errc::timeout)) {
      co_return ec;
    }
    if (ec && ec != core::make_error_code(core::errc::cancelled)) {
      co_return ec;
    }
  }
  co_return core::make_error_code(core::errc::cancelled);
}

}  // namespace secs::hsms
