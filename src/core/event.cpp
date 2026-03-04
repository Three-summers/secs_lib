#include "secs/core/event.hpp"

#include "secs/core/error.hpp"

#include <asio/as_tuple.hpp>
#include <asio/dispatch.hpp>
#include <asio/error.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

#include <list>
#include <mutex>
#include <new>
#include <vector>

namespace secs::core {

struct Event::State final {
    mutable std::mutex mu{};
    bool signaled{false};
    std::uint64_t set_generation{0};
    std::uint64_t cancel_generation{0};
    std::size_t max_waiters{1024};
    std::size_t waiter_count{0};
    std::list<std::weak_ptr<asio::steady_timer>> waiters{};
};

Event::Event() : state_(std::make_shared<State>()) {}

Event::Event(std::size_t max_waiters) : state_(std::make_shared<State>()) {
    state_->max_waiters = max_waiters;
}

bool Event::is_set() const noexcept {
    if (!state_) {
        return false;
    }
    try {
        std::lock_guard lk(state_->mu);
        return state_->signaled;
    } catch (...) {
        return false;
    }
}

/*
 * Event 的实现要点（理解 set/reset/cancel/timeout 的交互）：
 *
 * - 等待者通过 asio::steady_timer 挂起；
 * - set()/cancel() 需要“广播唤醒”所有等待者。这里采用“登记所有 waiter
 *   的 timer（以 weak_ptr 保存），set()/cancel() 时收集存活 timer 并逐个
 *   cancel”的方式实现广播唤醒。
 *
 * - 由于“取消”与“定时器到期”都会让 async_wait 返回，因此需要用两个
 * generation 计数区分来源：
 *   - set_generation_ 变化：说明被 set() 唤醒 -> 返回成功
 *   - cancel_generation_ 变化：说明被 cancel() 唤醒 -> 返回 cancelled
 *   - 两者都没变化：说明是“超时到期”或“底层错误”
 */
void Event::cancel_waiters_() noexcept {
    if (!state_) {
        return;
    }

    std::vector<std::shared_ptr<asio::steady_timer>> timers;
    try {
        std::lock_guard lk(state_->mu);
        timers.reserve(state_->waiters.size());

        // 注意：这里不在遍历 waiters_ 的同时调用 cancel()，以避免 cancel
        // 触发等待者立即恢复并重入修改 waiters_，导致迭代器失效/容器损坏。
        for (auto it = state_->waiters.begin(); it != state_->waiters.end();) {
            if (auto t = it->lock()) {
                timers.push_back(std::move(t));
                ++it;
            } else {
                // 清理已经结束/析构的等待者（weak_ptr 失效）。
                it = state_->waiters.erase(it);
            }
        }
    } catch (...) {
        // best-effort：异常时不影响主流程
    }

    for (const auto &t : timers) {
        try {
            // 取消操作需在 timer 自身 executor 上执行，避免跨线程/跨 strand 直接
            // 操作 Asio 对象导致未定义行为。
            asio::dispatch(t->get_executor(), [t]() noexcept { (void)t->cancel(); });
        } catch (...) {
            // cancel() 可能抛出 system_error；在 noexcept 路径中兜底吞掉。
        }
    }
}

void Event::set() noexcept {
    if (!state_) {
        return;
    }
    try {
        std::lock_guard lk(state_->mu);
        state_->signaled = true;
        ++state_->set_generation;
    } catch (...) {
        return;
    }
    cancel_waiters_();
}

void Event::reset() noexcept {
    if (!state_) {
        return;
    }
    try {
        std::lock_guard lk(state_->mu);
        state_->signaled = false;
    } catch (...) {
    }
}

void Event::cancel() noexcept {
    if (!state_) {
        return;
    }
    try {
        std::lock_guard lk(state_->mu);
        ++state_->cancel_generation;
    } catch (...) {
        return;
    }
    cancel_waiters_();
}

asio::awaitable<std::error_code>
Event::async_wait(std::optional<steady_clock::duration> timeout) {
    const auto state = state_;
    if (!state) {
        co_return make_error_code(errc::invalid_argument);
    }

    std::uint64_t local_set_gen = 0;
    std::uint64_t local_cancel_gen = 0;
    std::error_code wait_ec{};
    try {
        auto ex = co_await asio::this_coro::executor;
        auto timer = std::make_shared<asio::steady_timer>(ex);
        if (timeout.has_value()) {
            timer->expires_after(*timeout);
        } else {
            // 没有超时时间时，用一个“很远的时间点”模拟永久等待。
            timer->expires_at(asio::steady_timer::time_point::max());
        }

        std::list<std::weak_ptr<asio::steady_timer>>::iterator it;
        {
            std::lock_guard lk(state->mu);

            // 检查条件与登记 waiter 需要同锁完成，避免 set/cancel 在两步之间穿插造成丢唤醒。
            if (state->signaled) {
                co_return std::error_code{};
            }

            const auto max_waiters =
                state->max_waiters == 0 ? std::size_t{1} : state->max_waiters;
            if (state->waiter_count >= max_waiters) {
                co_return make_error_code(errc::out_of_memory);
            }
            ++state->waiter_count;

            // 记录当前 generation，用于在等待结束后判断“是 set/cancel
            // 导致的唤醒”还是“超时/错误”。
            local_set_gen = state->set_generation;
            local_cancel_gen = state->cancel_generation;
            try {
                it = state->waiters.insert(state->waiters.end(), timer);
            } catch (...) {
                --state->waiter_count;
                throw;
            }
        }

        struct WaiterCountGuard final {
            std::shared_ptr<State> state;
            ~WaiterCountGuard() {
                try {
                    std::lock_guard lk(state->mu);
                    --state->waiter_count;
                } catch (...) {
                }
            }
        } waiter_count_guard{state};

        struct WaiterListGuard final {
            std::shared_ptr<State> state;
            std::list<std::weak_ptr<asio::steady_timer>>::iterator it;
            ~WaiterListGuard() {
                try {
                    std::lock_guard lk(state->mu);
                    state->waiters.erase(it);
                } catch (...) {
                }
            }
        } waiter_list_guard{state, it};

        // 这里使用 as_tuple 使其返回错误码而不是异常。
        auto [ec] = co_await timer->async_wait(asio::as_tuple(asio::use_awaitable));
        wait_ec = ec;
    } catch (const std::bad_alloc &) {
        co_return make_error_code(errc::out_of_memory);
    } catch (...) {
        co_return make_error_code(errc::invalid_argument);
    }

    try {
        std::lock_guard lk(state->mu);
        if (state->set_generation != local_set_gen) {
            co_return std::error_code{};
        }
        if (state->cancel_generation != local_cancel_gen) {
            co_return make_error_code(errc::cancelled);
        }
    } catch (...) {
        co_return make_error_code(errc::invalid_argument);
    }

    if (!wait_ec) {
        // 定时器正常到期（没有错误码）-> errc::timeout
        co_return make_error_code(errc::timeout);
    }

    if (wait_ec == asio::error::operation_aborted) {
        // 定时器被 set()/cancel() 触发的“广播取消”唤醒 ->
        // errc::cancelled
        co_return make_error_code(errc::cancelled);
    }

    co_return wait_ec;
}

} // namespace secs::core
