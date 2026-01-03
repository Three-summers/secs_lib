#include "secs/secs1/link.hpp"

#include "secs/core/error.hpp"
#include "secs/core/event.hpp"
#include "secs/secs1/timer.hpp"

#include <deque>

namespace secs::secs1 {

/*
 * MemoryLink：用于单元测试的“内存串口”模拟。
 *
 * - create() 会生成一对 Endpoint（A/B），两端共享一份 SharedState：
 *   - a_to_b / b_to_a：模拟两个方向的字节队列
 *   - a_event / b_event：用于在队列由空变为非空时唤醒读协程
 *
 * - drop_next(n)：丢弃接下来写入的前 n 个字节（用于模拟丢包/噪声）
 * - set_fixed_delay(d)：为写入引入固定延迟（用于模拟串口时延与超时分支）
 *
 * 说明：该实现只用于测试场景，不追求高性能与强并发语义。
 */
struct MemoryLink::Endpoint::SharedState final {
    secs::core::Event a_event{};
    secs::core::Event b_event{};

    std::deque<secs::core::byte> a_to_b{};
    std::deque<secs::core::byte> b_to_a{};

    std::size_t drop_a_to_b{0};
    std::size_t drop_b_to_a{0};

    std::optional<secs::core::duration> delay_a_to_b{};
    std::optional<secs::core::duration> delay_b_to_a{};
};

MemoryLink::Endpoint::Endpoint(asio::any_io_executor ex,
                               std::shared_ptr<SharedState> shared,
                               bool is_a)
    : executor_(std::move(ex)), shared_(std::move(shared)), is_a_(is_a) {}

asio::any_io_executor MemoryLink::Endpoint::executor() const noexcept {
    return executor_;
}

void MemoryLink::Endpoint::drop_next(std::size_t n) noexcept {
    if (!shared_) {
        return;
    }
    if (is_a_) {
        shared_->drop_a_to_b = n;
    } else {
        shared_->drop_b_to_a = n;
    }
}

void MemoryLink::Endpoint::set_fixed_delay(
    std::optional<secs::core::duration> delay) noexcept {
    if (!shared_) {
        return;
    }
    if (is_a_) {
        shared_->delay_a_to_b = delay;
    } else {
        shared_->delay_b_to_a = delay;
    }
}

asio::awaitable<std::error_code>
MemoryLink::Endpoint::async_write(secs::core::bytes_view data) {
    if (!shared_) {
        co_return secs::core::make_error_code(
            secs::core::errc::invalid_argument);
    }

    const auto delay = is_a_ ? shared_->delay_a_to_b : shared_->delay_b_to_a;
    if (delay.has_value() && *delay != secs::core::duration::zero()) {
        Timer t(executor_);
        auto ec = co_await t.async_sleep(*delay);
        if (ec) {
            co_return ec;
        }
    }

    auto &out_queue = is_a_ ? shared_->a_to_b : shared_->b_to_a;
    auto &out_event = is_a_ ? shared_->b_event : shared_->a_event;
    auto &drop = is_a_ ? shared_->drop_a_to_b : shared_->drop_b_to_a;

    bool wrote_any = false;
    for (const auto b : data) {
        if (drop != 0) {
            --drop;
            continue;
        }
        out_queue.push_back(b);
        wrote_any = true;
    }

    if (wrote_any) {
        out_event.set();
    }

    co_return std::error_code{};
}

asio::awaitable<std::pair<std::error_code, secs::core::byte>>
MemoryLink::Endpoint::async_read_byte(
    std::optional<secs::core::duration> timeout) {
    if (!shared_) {
        co_return std::pair{
            secs::core::make_error_code(secs::core::errc::invalid_argument),
            secs::core::byte{0}};
    }

    auto &in_queue = is_a_ ? shared_->b_to_a : shared_->a_to_b;
    auto &in_event = is_a_ ? shared_->a_event : shared_->b_event;

    for (;;) {
        if (!in_queue.empty()) {
            const auto b = in_queue.front();
            in_queue.pop_front();
            if (in_queue.empty()) {
                // 队列被读空：复位事件，避免后续无意义的立即唤醒。
                in_event.reset();
            }
            co_return std::pair{std::error_code{}, b};
        }

        // 队列为空：等待对端写入并 set() 事件，或等待超时/取消。
        auto ec = co_await in_event.async_wait(timeout);
        if (ec) {
            co_return std::pair{ec, secs::core::byte{0}};
        }
    }
}

std::pair<MemoryLink::Endpoint, MemoryLink::Endpoint>
MemoryLink::create(asio::any_io_executor ex) {
    auto shared = std::make_shared<Endpoint::SharedState>();
    Endpoint a(ex, shared, true);
    Endpoint b(ex, shared, false);
    return {std::move(a), std::move(b)};
}

} // namespace secs::secs1
