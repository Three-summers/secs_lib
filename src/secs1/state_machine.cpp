#include "secs/secs1/state_machine.hpp"

#include "secs/core/error.hpp"

namespace secs::secs1 {
namespace {

[[nodiscard]] bool is_timeout(const std::error_code& ec) noexcept {
  return ec == secs::core::make_error_code(secs::core::errc::timeout);
}

}  // namespace

StateMachine::StateMachine(
  Link& link,
  std::optional<std::uint16_t> expected_device_id,
  Timeouts timeouts,
  std::size_t retry_limit)
  : link_(link),
    expected_device_id_(expected_device_id),
    timeouts_(timeouts),
    retry_limit_(retry_limit) {}

asio::awaitable<std::error_code> StateMachine::async_send_control(secs::core::byte b) {
  secs::core::byte tmp = b;
  co_return co_await link_.async_write(secs::core::bytes_view{&tmp, 1});
}

asio::awaitable<std::pair<std::error_code, secs::core::byte>> StateMachine::async_read_byte(
  std::optional<secs::core::duration> timeout) {
  co_return co_await link_.async_read_byte(timeout);
}

asio::awaitable<std::error_code> StateMachine::async_send(const Header& header, secs::core::bytes_view body) {
  if (state_ != State::idle) {
    co_return secs::core::make_error_code(secs::core::errc::invalid_argument);
  }

  state_ = State::wait_eot;

  bool handshake_ok = false;
  for (std::size_t attempt = 0; attempt < retry_limit_; ++attempt) {
    auto ec = co_await async_send_control(kEnq);
    if (ec) {
      state_ = State::idle;
      co_return ec;
    }

    auto [rec_ec, resp] = co_await async_read_byte(timeouts_.t2_protocol);
    if (!rec_ec && (resp == kEot || resp == kAck)) {
      handshake_ok = true;
      break;
    }
    if (!rec_ec && resp == kNak) {
      continue;
    }
    if (is_timeout(rec_ec)) {
      continue;
    }
    if (rec_ec) {
      state_ = State::idle;
      co_return rec_ec;
    }
    state_ = State::idle;
    co_return make_error_code(errc::protocol_error);
  }

  if (!handshake_ok) {
    state_ = State::idle;
    co_return make_error_code(errc::too_many_retries);
  }

  auto frames = fragment_message(header, body);

  for (const auto& frame : frames) {
    state_ = State::wait_check;
    std::size_t attempts = 0;

    for (;;) {
      auto ec = co_await link_.async_write(secs::core::bytes_view{frame.data(), frame.size()});
      if (ec) {
        state_ = State::idle;
        co_return ec;
      }

      auto [rec_ec, resp] = co_await async_read_byte(timeouts_.t2_protocol);
      if (!rec_ec && resp == kAck) {
        break;
      }
      if ((!rec_ec && resp == kNak) || is_timeout(rec_ec)) {
        ++attempts;
        if (attempts >= retry_limit_) {
          state_ = State::idle;
          co_return make_error_code(errc::too_many_retries);
        }
        continue;
      }
      if (rec_ec) {
        state_ = State::idle;
        co_return rec_ec;
      }
      state_ = State::idle;
      co_return make_error_code(errc::protocol_error);
    }
  }

  state_ = State::idle;
  co_return std::error_code{};
}

asio::awaitable<std::pair<std::error_code, ReceivedMessage>> StateMachine::async_receive(
  std::optional<secs::core::duration> timeout) {
  if (state_ != State::idle) {
    co_return std::pair{secs::core::make_error_code(secs::core::errc::invalid_argument), ReceivedMessage{}};
  }

  // 等待对端 ENQ（忽略噪声字符）
  for (;;) {
    auto [ec, b] = co_await async_read_byte(timeout);
    if (ec) {
      co_return std::pair{ec, ReceivedMessage{}};
    }
    if (b == kEnq) {
      break;
    }
  }

  state_ = State::wait_block;

  // 默认总是允许对方发送（若未来需要 busy/拒绝，可在这里发送 NAK）
  {
    auto ec = co_await async_send_control(kEot);
    if (ec) {
      state_ = State::idle;
      co_return std::pair{ec, ReceivedMessage{}};
    }
  }

  Reassembler re(expected_device_id_);
  std::size_t nack_count = 0;

  auto next_block_timeout = timeouts_.t2_protocol;
  while (!re.has_message()) {
    // Length（第 1 个 block 用 T2；后续 block 用 T4）
    auto [len_ec, len_b] = co_await async_read_byte(next_block_timeout);
    if (len_ec) {
      state_ = State::idle;
      co_return std::pair{len_ec, ReceivedMessage{}};
    }

    const auto length = static_cast<std::size_t>(len_b);
    if (length < kHeaderSize || length > kMaxBlockLength) {
      (void)co_await async_send_control(kNak);
      state_ = State::idle;
      co_return std::pair{make_error_code(errc::invalid_block), ReceivedMessage{}};
    }

    std::vector<secs::core::byte> frame;
    frame.reserve(1 + length + 2);
    frame.push_back(len_b);

    // 读取 Header+Data+Checksum，按字节应用 T1（字符间超时）
    for (std::size_t i = 0; i < length + 2; ++i) {
      auto [b_ec, b] = co_await async_read_byte(timeouts_.t1_intercharacter);
      if (b_ec) {
        state_ = State::idle;
        co_return std::pair{b_ec, ReceivedMessage{}};
      }
      frame.push_back(b);
    }

    DecodedBlock decoded{};
    auto dec_ec = decode_block(secs::core::bytes_view{frame.data(), frame.size()}, decoded);
    if (dec_ec) {
      (void)co_await async_send_control(kNak);
      ++nack_count;
      if (nack_count >= retry_limit_) {
        state_ = State::idle;
        co_return std::pair{make_error_code(errc::too_many_retries), ReceivedMessage{}};
      }
      continue;
    }

    auto acc_ec = re.accept(decoded);
    if (acc_ec) {
      (void)co_await async_send_control(kNak);
      state_ = State::idle;
      co_return std::pair{acc_ec, ReceivedMessage{}};
    }

    // block 校验通过
    nack_count = 0;
    (void)co_await async_send_control(kAck);
    next_block_timeout = timeouts_.t4_interblock;
  }

  state_ = State::idle;

  ReceivedMessage msg{};
  msg.header = re.message_header();
  auto body_view = re.message_body();
  msg.body.assign(body_view.begin(), body_view.end());
  co_return std::pair{std::error_code{}, std::move(msg)};
}

asio::awaitable<std::pair<std::error_code, ReceivedMessage>> StateMachine::async_transact(
  const Header& header,
  secs::core::bytes_view body) {
  auto ec = co_await async_send(header, body);
  if (ec) {
    co_return std::pair{ec, ReceivedMessage{}};
  }
  co_return co_await async_receive(timeouts_.t3_reply);
}

}  // namespace secs::secs1
