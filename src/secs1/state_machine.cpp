#include "secs/secs1/state_machine.hpp"

#include "secs/core/error.hpp"

namespace secs::secs1 {
namespace {

[[nodiscard]] bool is_timeout(const std::error_code &ec) noexcept {
    return ec == secs::core::make_error_code(secs::core::errc::timeout);
}

} // namespace

/*
 * SECS-I 传输层状态机（SEMI E4）要点：
 *
 * - 发送流程（async_send）：
 *   1) 发 ENQ，请求占用半双工链路
 *   2) 在 T2 内等待对端 EOT/ACK（NAK/超时会触发重试）
 *   3) 将消息按 243B/块切分为多个“块帧”
 *   4) 逐块发送帧，并在 T2 内等待 ACK/NAK（NAK/超时会重传该块）
 *
 * - 接收流程（async_receive）：
 *   1) 等待对端 ENQ（忽略噪声字节）
 *   2) 回 EOT，表示允许对端开始发送
 *   3) 逐块读取：长度字段（首块 T2，后续块 T4）+（头部+数据+校验和）（逐字节
 * T1） 4) 校验/重组成功回 ACK，失败回 NAK；直到收到 end_bit 的最后一块
 */

StateMachine::StateMachine(Link &link,
                           std::optional<std::uint16_t> expected_device_id,
                           Timeouts timeouts,
                           std::size_t retry_limit)
    : link_(link), expected_device_id_(expected_device_id), timeouts_(timeouts),
      retry_limit_(retry_limit) {}

asio::awaitable<std::error_code>
StateMachine::async_send_control(secs::core::byte b) {
    secs::core::byte tmp = b;
    co_return co_await link_.async_write(secs::core::bytes_view{&tmp, 1});
}

asio::awaitable<std::pair<std::error_code, secs::core::byte>>
StateMachine::async_read_byte(std::optional<secs::core::duration> timeout) {
    co_return co_await link_.async_read_byte(timeout);
}

asio::awaitable<std::error_code>
StateMachine::async_send(const Header &header, secs::core::bytes_view body) {
    if (state_ != State::idle) {
        co_return secs::core::make_error_code(
            secs::core::errc::invalid_argument);
    }

    // 进入“等待对端允许发送”的阶段（ENQ -> 等待 EOT/ACK）。
    state_ = State::wait_eot;

    bool handshake_ok = false;
    for (std::size_t attempt = 0; attempt < retry_limit_; ++attempt) {
        // 发 ENQ：请求占用链路。
        auto ec = co_await async_send_control(kEnq);
        if (ec) {
            state_ = State::idle;
            co_return ec;
        }

        // 等待对端响应（T2）。EOT/ACK 视为允许发送；NAK/超时则重试。
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

    // SECS-I 规定单个块的数据最大 243 字节：这里把 body 切分并编码成多个帧。
    auto frames = fragment_message(header, body);

    for (const auto &frame : frames) {
        state_ = State::wait_check;
        std::size_t attempts = 0;

        for (;;) {
            // 发送 1 个完整帧，然后等待 ACK/NAK（T2）。
            auto ec = co_await link_.async_write(
                secs::core::bytes_view{frame.data(), frame.size()});
            if (ec) {
                state_ = State::idle;
                co_return ec;
            }

            auto [rec_ec, resp] =
                co_await async_read_byte(timeouts_.t2_protocol);
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

asio::awaitable<std::pair<std::error_code, ReceivedMessage>>
StateMachine::async_receive(std::optional<secs::core::duration> timeout) {
    if (state_ != State::idle) {
        co_return std::pair{
            secs::core::make_error_code(secs::core::errc::invalid_argument),
            ReceivedMessage{}};
    }

    // 等待对端 ENQ（忽略噪声字节）
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

    // 默认总是允许对方发送（若未来需要“忙/拒绝”，可在这里发送 NAK）
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
        // 长度字段（第 1 个块用 T2；后续块用 T4）
        auto [len_ec, len_b] = co_await async_read_byte(next_block_timeout);
        if (len_ec) {
            state_ = State::idle;
            co_return std::pair{len_ec, ReceivedMessage{}};
        }

        const auto length = static_cast<std::size_t>(len_b);
        if (length < kHeaderSize || length > kMaxBlockLength) {
            (void)co_await async_send_control(kNak);
            state_ = State::idle;
            co_return std::pair{make_error_code(errc::invalid_block),
                                ReceivedMessage{}};
        }

        std::vector<secs::core::byte> frame;
        frame.reserve(1 + length + 2);
        frame.push_back(len_b);

        // 读取“头部+数据+校验和”，按字节应用 T1（字符间超时）。
        for (std::size_t i = 0; i < length + 2; ++i) {
            auto [b_ec, b] =
                co_await async_read_byte(timeouts_.t1_intercharacter);
            if (b_ec) {
                state_ = State::idle;
                co_return std::pair{b_ec, ReceivedMessage{}};
            }
            frame.push_back(b);
        }

        DecodedBlock decoded{};
        auto dec_ec = decode_block(
            secs::core::bytes_view{frame.data(), frame.size()}, decoded);
        if (dec_ec) {
            (void)co_await async_send_control(kNak);
            ++nack_count;
            if (nack_count >= retry_limit_) {
                state_ = State::idle;
                co_return std::pair{make_error_code(errc::too_many_retries),
                                    ReceivedMessage{}};
            }
            continue;
        }

        auto acc_ec = re.accept(decoded);
        if (acc_ec) {
            (void)co_await async_send_control(kNak);
            state_ = State::idle;
            co_return std::pair{acc_ec, ReceivedMessage{}};
        }

        // 当前块校验通过
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

asio::awaitable<std::pair<std::error_code, ReceivedMessage>>
StateMachine::async_transact(const Header &header,
                             secs::core::bytes_view body) {
    auto ec = co_await async_send(header, body);
    if (ec) {
        co_return std::pair{ec, ReceivedMessage{}};
    }
    co_return co_await async_receive(timeouts_.t3_reply);
}

} // namespace secs::secs1
