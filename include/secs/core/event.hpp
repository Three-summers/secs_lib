#pragma once

#include "secs/core/common.hpp"

#include <asio/awaitable.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <system_error>

namespace secs::core {

/**
 * @brief 协程可等待事件（set/reset/cancel + timeout）。
 * 将其作为同步原语，async_wait 时会等待，直到 cancel 或者 set 或者 timeout
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
    Event();
    explicit Event(std::size_t max_waiters);

    Event(const Event &) = delete;
    Event &operator=(const Event &) = delete;
    Event(Event &&) noexcept = default;
    Event &operator=(Event &&) noexcept = default;

    void set() noexcept;
    void reset() noexcept;
    void cancel() noexcept;

    [[nodiscard]] bool is_set() const noexcept;

    asio::awaitable<std::error_code>
    async_wait(std::optional<steady_clock::duration> timeout = std::nullopt);

private:
    void cancel_waiters_() noexcept;

    struct State;
    std::shared_ptr<State> state_;
};

} // namespace secs::core
