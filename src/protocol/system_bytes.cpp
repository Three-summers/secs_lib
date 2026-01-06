#include "secs/protocol/system_bytes.hpp"

#include "secs/core/error.hpp"

#include <algorithm>
#include <limits>
#include <new>

namespace secs::protocol {
namespace {

constexpr std::uint32_t kMinSystemBytes = 1U;

[[nodiscard]] bool is_valid_system_bytes(std::uint32_t sb) noexcept {
    return sb != 0U;
}

} // namespace

/*
 * SystemBytes 分配策略：
 * - 0 作为保留值永不分配（HSMS/SECS-I 的 SystemBytes 语义中通常不使用 0）
 * - 优先复用已释放的值（free_ 队列）
 * - 否则从 next_ 递增寻找未占用值，达到 max_ 后回绕到 1
 * - 用 in_use_ 集合防止“同一时刻重复分配”
 */
SystemBytes::SystemBytes(std::uint32_t initial,
                         std::uint32_t max_value) noexcept
    : next_(initial), max_(max_value) {
    if (max_ == 0U) {
        max_ = std::numeric_limits<std::uint32_t>::max();
    }
    if (max_ < kMinSystemBytes) {
        max_ = kMinSystemBytes;
    }

    if (next_ == 0U || next_ > max_) {
        next_ = kMinSystemBytes;
    }
}

std::uint32_t SystemBytes::next_candidate_() noexcept {
    const auto current = next_;
    if (next_ == max_) {
        next_ = kMinSystemBytes;
    } else {
        next_ += 1U;
    }
    return current;
}

std::error_code SystemBytes::allocate(std::uint32_t &out) noexcept {
    try {
        std::lock_guard lk(mu_);

        if (!free_.empty()) {
            const auto sb = free_.front();
            free_.pop_front();
            in_use_.insert(sb);
            out = sb;
            return std::error_code{};
        }

        const auto max_usable = static_cast<std::size_t>(max_);
        if (in_use_.size() >= max_usable) {
            return secs::core::make_error_code(secs::core::errc::buffer_overflow);
        }

        // 正常情况下 in_use 很小，这里最多尝试 in_use.size()+2 次即可找到空闲值。
        const std::size_t attempts =
            std::min<std::size_t>(in_use_.size() + 2U, max_usable);
        for (std::size_t i = 0; i < attempts; ++i) {
            const auto candidate = next_candidate_();
            if (in_use_.insert(candidate).second) {
                out = candidate;
                return std::error_code{};
            }
        }

        return secs::core::make_error_code(secs::core::errc::buffer_overflow);
    } catch (const std::bad_alloc &) {
        return secs::core::make_error_code(secs::core::errc::out_of_memory);
    } catch (...) {
        return secs::core::make_error_code(secs::core::errc::invalid_argument);
    }
}

void SystemBytes::release(std::uint32_t system_bytes) noexcept {
    if (!is_valid_system_bytes(system_bytes) || system_bytes > max_) {
        return;
    }
    try {
        std::lock_guard lk(mu_);
        if (in_use_.erase(system_bytes) == 0U) {
            return;
        }
        try {
            free_.push_back(system_bytes);
        } catch (...) {
            // 释放队列追加失败（例如 OOM）：允许丢弃该 system_bytes，避免 noexcept
            // 路径触发 std::terminate。
        }
    } catch (...) {
        // 兜底：mutex/容器操作异常时吞掉，避免 noexcept 路径触发 std::terminate。
    }
}

bool SystemBytes::is_in_use(std::uint32_t system_bytes) const noexcept {
    if (!is_valid_system_bytes(system_bytes) || system_bytes > max_) {
        return false;
    }
    try {
        std::lock_guard lk(mu_);
        return in_use_.find(system_bytes) != in_use_.end();
    } catch (...) {
        return false;
    }
}

std::size_t SystemBytes::in_use_count() const noexcept {
    try {
        std::lock_guard lk(mu_);
        return in_use_.size();
    } catch (...) {
        return 0;
    }
}

} // namespace secs::protocol
