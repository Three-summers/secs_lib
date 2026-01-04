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
 * - Block 分包/重组（≤244B/block）
 * - Checksum/DeviceID 校验（失败返回 NAK）
 * - T1/T2/T3/T4 超时
 *
 * 并发与线程安全（非常重要）：
 * - SECS-I 是“半双工 + 字节流”，同一条 Link 在同一时刻只能由一个协程驱动读写。
 * - 本类不做内部互斥/排队：当 state()!=idle 时再次调用
 *   async_send/async_receive/async_transact 会返回 `invalid_argument`。
 *   这样做的目的，是避免并发读写导致字节流交错（例如把对端的 ENQ/ACK 当成自己
 *   正在等待的字节），从而引发难以定位的协议错误。
 * - 若你的 Host/Equipment 是多线程：请在上层把所有对同一个 StateMachine/Link 的
 *   调用串行化（例如使用 `asio::strand`，或在业务层加发送队列/互斥锁）。
 *
 * 与你提到的“多线程 Host”典型问题的关系：
 * - 正常情况下：发送方发出一个 block frame 后，接收方必须先回 ACK/NAK，
 *   再能进入新的 ENQ 发送流程。
 * - 若对端实现违规（例如多线程并发写串口，导致在 ACK 之前就发送 ENQ），
 *   本实现会把这种字节序列视为协议错误并返回相应 error_code。
 */
class StateMachine final {
public:
    explicit StateMachine(
        Link &link,
        std::optional<std::uint16_t> expected_device_id = std::nullopt,
        Timeouts timeouts = {},
        std::size_t retry_limit = 3);

    [[nodiscard]] asio::any_io_executor executor() const noexcept {
        return link_.executor();
    }

    [[nodiscard]] State state() const noexcept { return state_; }
    [[nodiscard]] const Timeouts &timeouts() const noexcept {
        return timeouts_;
    }

    asio::awaitable<std::error_code> async_send(const Header &header,
                                                secs::core::bytes_view body);

    asio::awaitable<std::pair<std::error_code, ReceivedMessage>>
    async_receive(std::optional<secs::core::duration> timeout = std::nullopt);

    asio::awaitable<std::pair<std::error_code, ReceivedMessage>>
    async_transact(const Header &header, secs::core::bytes_view body);

private:
    asio::awaitable<std::error_code> async_send_control(secs::core::byte b);

    asio::awaitable<std::pair<std::error_code, secs::core::byte>>
    async_read_byte(std::optional<secs::core::duration> timeout);

    Link &link_;
    std::optional<std::uint16_t> expected_device_id_{};
    Timeouts timeouts_{};
    std::size_t retry_limit_{3};
    State state_{State::idle};
};

} // namespace secs::secs1
