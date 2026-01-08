#include "secs/secs1/state_machine.hpp"

#include "secs/core/error.hpp"

#include <algorithm>
#include <spdlog/spdlog.h>

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
 *   3) 将消息按 244B/块切分为多个“块帧”
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
        // 并发保护：
        // SECS-I 是半双工字节流；若同时有两个协程/线程驱动同一条 Link：
        // - 写入会交错，破坏 frame；
        // - 读取会错配，把对端控制字节误判为自己等待的响应。
        // 因此这里要求“同一时刻只能有一个操作在跑”。
        co_return secs::core::make_error_code(
            secs::core::errc::invalid_argument);
    }

    if (!body.empty()) {
        const auto blocks =
            (body.size() + kMaxBlockDataSize - 1) / kMaxBlockDataSize;
        if (blocks > 0x7FFFu) {
            co_return secs::core::make_error_code(
                secs::core::errc::invalid_argument);
        }
    }

    SPDLOG_DEBUG(
        "secs1 async_send start: dev_id={} rbit={} S{}F{} W={} sb={} body_n={}",
        header.device_id,
        header.reverse_bit ? 1 : 0,
        static_cast<int>(header.stream),
        static_cast<int>(header.function),
        header.wait_bit ? 1 : 0,
        header.system_bytes,
        body.size());

    // SECS-I 规定单个块的数据最大 244 字节：这里把 body 切分并编码成多个帧。
    auto frames = fragment_message(header, body);

    for (const auto &frame : frames) {
        // 注意：兼容更多 SECS-I 实现，这里按“每个块都执行一次 ENQ/EOT 握手”的方式
        // 发送。单块消息与旧行为一致；多块消息会在 ACK 后再次 ENQ。
        state_ = State::wait_eot;

        bool handshake_ok = false;
        for (std::size_t attempt = 0; attempt < retry_limit_; ++attempt) {
            // 发 ENQ：请求占用链路。
            auto ec = co_await async_send_control(kEnq);
            if (ec) {
                SPDLOG_DEBUG("secs1 async_send ENQ failed: ec={}({})",
                             ec.value(),
                             ec.message());
                state_ = State::idle;
                co_return ec;
            }

            // 等待对端响应（T2）。EOT/ACK 视为允许发送；NAK/超时则重试。
            auto [rec_ec, resp] =
                co_await async_read_byte(timeouts_.t2_protocol);
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
                SPDLOG_DEBUG(
                    "secs1 async_send handshake receive failed: ec={}({})",
                    rec_ec.value(),
                    rec_ec.message());
                state_ = State::idle;
                co_return rec_ec;
            }
            state_ = State::idle;
            SPDLOG_DEBUG(
                "secs1 async_send handshake protocol_error (resp=0x{:02X})",
                static_cast<unsigned int>(resp));
            co_return make_error_code(errc::protocol_error);
        }

        if (!handshake_ok) {
            state_ = State::idle;
            SPDLOG_DEBUG("secs1 async_send handshake too_many_retries");
            co_return make_error_code(errc::too_many_retries);
        }

        state_ = State::wait_check;
        std::size_t attempts = 0;

        for (;;) {
            // 发送 1 个完整帧，然后等待 ACK/NAK（T2）。
            auto ec = co_await link_.async_write(
                secs::core::bytes_view{frame.data(), frame.size()});
            if (ec) {
                SPDLOG_DEBUG("secs1 async_send frame write failed: ec={}({})",
                             ec.value(),
                             ec.message());
                state_ = State::idle;
                co_return ec;
            }

            auto [rec_ec, resp] =
                co_await async_read_byte(timeouts_.t2_protocol);
            if (!rec_ec && resp == kAck) {
                break;
            }
            // 注意：这里严格期待 ACK/NAK（或超时触发重传）。
            // 若对端实现违规（例如多线程并发写串口，在 ACK 之前就发送 ENQ），
            // 这里可能读到 ENQ；这会被判为协议错误并终止本次发送。
            if ((!rec_ec && resp == kNak) || is_timeout(rec_ec)) {
                ++attempts;
                if (attempts >= retry_limit_) {
                    state_ = State::idle;
                    SPDLOG_DEBUG("secs1 async_send frame too_many_retries");
                    co_return make_error_code(errc::too_many_retries);
                }
                continue;
            }
            if (rec_ec) {
                SPDLOG_DEBUG("secs1 async_send frame receive failed: ec={}({})",
                             rec_ec.value(),
                             rec_ec.message());
                state_ = State::idle;
                co_return rec_ec;
            }
            state_ = State::idle;
            SPDLOG_DEBUG("secs1 async_send frame protocol_error (resp=0x{:02X})",
                         static_cast<unsigned int>(resp));
            co_return make_error_code(errc::protocol_error);
        }
    }

    state_ = State::idle;
    SPDLOG_DEBUG("secs1 async_send done");
    co_return std::error_code{};
}

asio::awaitable<std::pair<std::error_code, ReceivedMessage>>
StateMachine::async_receive(std::optional<secs::core::duration> timeout) {
    if (state_ != State::idle) {
        co_return std::pair{
            secs::core::make_error_code(secs::core::errc::invalid_argument),
            ReceivedMessage{}};
    }

    // 启动接收：
    // - 若当前没有任何 in-flight 消息：按标准流程等待 ENQ，再回 EOT；
    // - 若已有 in-flight（多 Block interleaving）：下一块可能以 ENQ 或 Length 开头。
    bool need_wait_enq = in_flight_.empty();
    if (need_wait_enq) {
        // 等待对端 ENQ（忽略噪声字节）
        for (;;) {
            auto [ec, b] = co_await async_read_byte(timeout);
            if (ec) {
                SPDLOG_DEBUG(
                    "secs1 async_receive failed while waiting ENQ: ec={}({})",
                    ec.value(),
                    ec.message());
                co_return std::pair{ec, ReceivedMessage{}};
            }
            if (b == kEnq) {
                break;
            }
        }

        SPDLOG_DEBUG("secs1 async_receive got ENQ");

        state_ = State::wait_block;

        // 默认总是允许对方发送（若未来需要“忙/拒绝”，可在这里发送 NAK）
        auto ec = co_await async_send_control(kEot);
        if (ec) {
            SPDLOG_DEBUG("secs1 async_receive send EOT failed: ec={}({})",
                         ec.value(),
                         ec.message());
            state_ = State::idle;
            co_return std::pair{ec, ReceivedMessage{}};
        }
    } else {
        state_ = State::wait_block;
    }

    std::size_t nack_count = 0; // decode_block 失败的连续次数（无 header 时无法路由）
    bool allow_enq_or_length = !need_wait_enq;
    auto next_block_timeout =
        need_wait_enq ? timeouts_.t2_protocol : timeouts_.t4_interblock;

    for (;;) {
        // 多块消息的“块间启动方式”存在不同实现：
        // - 一些实现：每个块都重新 ENQ/EOT；
        // - 一些实现：只在消息开始 ENQ/EOT，后续块直接发送 Length 开头的块帧。
        //
        // 为提升互操作性：当链路上已经存在“正在进行中的多块消息”时，允许两种方式：
        // - 若看到 ENQ，则发送 EOT 再读块帧；
        // - 若看到合法 Length（10..254），则按“直接块帧”处理。
        std::optional<secs::core::byte> first_len{};
        if (allow_enq_or_length) {
            const auto deadline = secs::core::steady_clock::now() + next_block_timeout;
            for (;;) {
                const auto now = secs::core::steady_clock::now();
                if (now >= deadline) {
                    // Block 间超时：认为多块消息接收已中断，丢弃全部 in-flight 状态。
                    in_flight_.clear();
                    state_ = State::idle;
                    co_return std::pair{
                        secs::core::make_error_code(secs::core::errc::timeout),
                        ReceivedMessage{}};
                }

                const auto remaining = deadline - now;
                auto [ec, b] = co_await async_read_byte(remaining);
                if (ec) {
                    SPDLOG_DEBUG(
                        "secs1 async_receive waiting next block start failed: ec={}({})",
                        ec.value(),
                        ec.message());
                    in_flight_.clear();
                    state_ = State::idle;
                    co_return std::pair{ec, ReceivedMessage{}};
                }

                if (b == kEnq) {
                    // 对端请求发送下一块：回 EOT 表示允许发送。
                    auto eot_ec = co_await async_send_control(kEot);
                    if (eot_ec) {
                        SPDLOG_DEBUG(
                            "secs1 async_receive send EOT(for next block) failed: ec={}({})",
                            eot_ec.value(),
                            eot_ec.message());
                        in_flight_.clear();
                        state_ = State::idle;
                        co_return std::pair{eot_ec, ReceivedMessage{}};
                    }
                    next_block_timeout = timeouts_.t2_protocol;
                    break;
                }

                const auto len = static_cast<std::size_t>(b);
                if (len >= kHeaderSize && len <= kMaxBlockLength) {
                    first_len = b;
                    break;
                }

                // 其他字节：视为噪声/控制字符，继续等待（不立即失败）。
            }
        }

        // 长度字段（第 1 个块用 T2；后续块用 T4）
        std::error_code len_ec{};
        secs::core::byte len_b = 0;
        if (first_len.has_value()) {
            len_b = *first_len;
        } else {
            auto [ec_tmp, b_tmp] = co_await async_read_byte(next_block_timeout);
            len_ec = ec_tmp;
            len_b = b_tmp;
        }
        if (len_ec) {
            SPDLOG_DEBUG("secs1 async_receive length read failed: ec={}({})",
                         len_ec.value(),
                         len_ec.message());
            in_flight_.clear();
            state_ = State::idle;
            co_return std::pair{len_ec, ReceivedMessage{}};
        }

        const auto length = static_cast<std::size_t>(len_b);
        if (length < kHeaderSize || length > kMaxBlockLength) {
            (void)co_await async_send_control(kNak);
            in_flight_.clear();
            state_ = State::idle;
            SPDLOG_DEBUG("secs1 async_receive invalid length: {}", length);
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
                SPDLOG_DEBUG("secs1 async_receive frame byte read failed: ec={}({})",
                             b_ec.value(),
                             b_ec.message());
                in_flight_.clear();
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
                in_flight_.clear();
                state_ = State::idle;
                SPDLOG_DEBUG("secs1 async_receive too_many_retries (decode)");
                co_return std::pair{make_error_code(errc::too_many_retries),
                                    ReceivedMessage{}};
            }
            continue;
        }

        if (last_accepted_header_.has_value() &&
            decoded.header.reverse_bit == last_accepted_header_->reverse_bit &&
            decoded.header.device_id == last_accepted_header_->device_id &&
            decoded.header.wait_bit == last_accepted_header_->wait_bit &&
            decoded.header.stream == last_accepted_header_->stream &&
            decoded.header.function == last_accepted_header_->function &&
            decoded.header.end_bit == last_accepted_header_->end_bit &&
            decoded.header.block_number == last_accepted_header_->block_number &&
            decoded.header.system_bytes == last_accepted_header_->system_bytes) {
            const bool data_same =
                decoded.data.size() == last_accepted_data_.size() &&
                std::equal(decoded.data.begin(),
                           decoded.data.end(),
                           last_accepted_data_.begin());

            if (data_same) {
                // 重复块：典型原因是对端未收到 ACK 而重发；按 E4 语义丢弃并再次 ACK。
                nack_count = 0;
                (void)co_await async_send_control(kAck);
                next_block_timeout = timeouts_.t4_interblock;
                allow_enq_or_length = true;
                continue;
            }
            // Header 相同但 data 不同：
            // - 可能是上层复用 system_bytes 后发送的新消息（合法场景）；
            // - checksum 已通过，因此这里不要误判为协议错误。
            // 处理策略：按“新块”继续走正常重组流程。
        }

        const auto sb = decoded.header.system_bytes;
        auto it = in_flight_.find(sb);
        if (it == in_flight_.end()) {
            // interleaving：允许“新消息”在旧消息未结束时插入，但必须从 BlockNumber=1 开始。
            if (decoded.header.block_number != 1) {
                (void)co_await async_send_control(kNak);
                in_flight_.clear();
                state_ = State::idle;
                co_return std::pair{make_error_code(errc::block_sequence_error),
                                    ReceivedMessage{}};
            }
            InFlight st{};
            st.re = Reassembler(expected_device_id_);
            st.last_block = secs::core::steady_clock::now();
            it = in_flight_.emplace(sb, std::move(st)).first;
        }

        auto acc_ec = it->second.re.accept(decoded);
        if (acc_ec) {
            (void)co_await async_send_control(kNak);
            in_flight_.clear();
            state_ = State::idle;
            co_return std::pair{acc_ec, ReceivedMessage{}};
        }
        it->second.last_block = secs::core::steady_clock::now();

        // 当前块校验通过
        nack_count = 0;
        last_accepted_header_ = decoded.header;
        last_accepted_data_.assign(decoded.data.begin(), decoded.data.end());
        (void)co_await async_send_control(kAck);
        next_block_timeout = timeouts_.t4_interblock;
        allow_enq_or_length = true;

        if (it->second.re.has_message()) {
            ReceivedMessage msg{};
            msg.header = it->second.re.message_header();
            auto body_view = it->second.re.message_body();
            msg.body.assign(body_view.begin(), body_view.end());
            in_flight_.erase(it);
            state_ = State::idle;
            co_return std::pair{std::error_code{}, std::move(msg)};
        }
    }
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
