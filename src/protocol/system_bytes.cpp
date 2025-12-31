#include "secs/protocol/system_bytes.hpp"

#include "secs/core/error.hpp"

#include <limits>

namespace secs::protocol {
namespace {

constexpr std::uint32_t kMinSystemBytes = 1U;

[[nodiscard]] bool is_valid_system_bytes(std::uint32_t sb) noexcept {
  return sb != 0U;
}

}  // 匿名命名空间

/*
 * SystemBytes 分配策略：
 * - 0 作为保留值永不分配（HSMS/SECS-I 的 SystemBytes 语义中通常不使用 0）
 * - 优先复用已释放的值（free_ 队列）
 * - 否则从 next_ 递增寻找未占用值，达到 UINT32_MAX 后回绕到 1
 * - 用 in_use_ 集合防止“同一时刻重复分配”
 */
SystemBytes::SystemBytes(std::uint32_t initial) noexcept : next_(initial) {
  if (next_ == 0U) {
    next_ = kMinSystemBytes;
  }
}

std::uint32_t SystemBytes::next_candidate_() noexcept {
  const auto current = next_;
  if (next_ == std::numeric_limits<std::uint32_t>::max()) {
    next_ = kMinSystemBytes;
  } else {
    next_ += 1U;
  }
  return current;
}

std::error_code SystemBytes::allocate(std::uint32_t& out) noexcept {
  std::lock_guard lk(mu_);

  if (!free_.empty()) {
    const auto sb = free_.front();
    free_.pop_front();
    in_use_.insert(sb);
    out = sb;
    return std::error_code{};
  }

  constexpr std::uint64_t kMaxUsable = static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max());
  if (in_use_.size() >= (kMaxUsable - 1U)) {  // GCOVR_EXCL_LINE：极端分支（2^32-1 在用）难以在单测覆盖
    return secs::core::make_error_code(secs::core::errc::buffer_overflow);
  }

  // 正常情况下 in_use 很小，这里最多尝试 in_use.size()+2 次即可找到空闲值。
  const std::size_t attempts = in_use_.size() + 2U;
  for (std::size_t i = 0; i < attempts; ++i) {
    const auto candidate = next_candidate_();
    if (in_use_.insert(candidate).second) {
      out = candidate;
      return std::error_code{};
    }
  }

  return secs::core::make_error_code(secs::core::errc::buffer_overflow);
}

void SystemBytes::release(std::uint32_t system_bytes) noexcept {
  if (!is_valid_system_bytes(system_bytes)) {
    return;
  }

  std::lock_guard lk(mu_);
  if (in_use_.erase(system_bytes) == 0U) {
    return;
  }
  free_.push_back(system_bytes);
}

bool SystemBytes::is_in_use(std::uint32_t system_bytes) const noexcept {
  std::lock_guard lk(mu_);
  return in_use_.find(system_bytes) != in_use_.end();
}

std::size_t SystemBytes::in_use_count() const noexcept {
  std::lock_guard lk(mu_);
  return in_use_.size();
}

}  // 命名空间 secs::protocol
