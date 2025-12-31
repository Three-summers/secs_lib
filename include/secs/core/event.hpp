#pragma once

#include "secs/core/common.hpp"
#include "secs/core/error.hpp"

#include <asio/as_tuple.hpp>
#include <asio/awaitable.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

#include <cstdint>
#include <list>
#include <memory>
#include <optional>
#include <system_error>

namespace secs::core {

/**
 * @brief 协程可等待事件（set/reset/cancel + timeout）。
 *
 * 语义：
 * - set(): 置位并唤醒所有等待者；后续 wait 立即返回成功
 * - reset(): 清除置位状态；后续 wait 会阻塞/直到 set 或 timeout/cancel
 * - cancel(): 取消当前所有等待者（返回 cancelled），但不置位
 *
 * 注意：
 * - 本类默认假设在同一执行器/线程语境下使用；跨线程请自行用
 * strand/调度保证顺序。
 * - async_wait 使用 as_tuple(use_awaitable) 避免异常路径。
 */
class Event final {
public:
    Event() = default;

    void set() noexcept;
    void reset() noexcept;
    void cancel() noexcept;

    [[nodiscard]] bool is_set() const noexcept { return signaled_; }

    asio::awaitable<std::error_code>
    async_wait(std::optional<steady_clock::duration> timeout = std::nullopt);

private:
    void cancel_waiters_() noexcept;

    bool signaled_{false};
    std::uint64_t set_generation_{0};
    std::uint64_t cancel_generation_{0};

    std::list<std::shared_ptr<asio::steady_timer>> waiters_{};
};

} // namespace secs::core
