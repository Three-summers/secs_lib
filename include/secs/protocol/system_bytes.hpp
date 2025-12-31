#pragma once

#include "secs/core/error.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <system_error>
#include <unordered_set>

namespace secs::protocol {

/**
 * @brief SystemBytes 分配器：负责唯一性、在用追踪、释放重用与 wrap-around
 * 处理。
 *
 * 说明：
 * - SystemBytes 在 HSMS/SECS-I 中用于请求-响应关联（response 回显 request 的
 * SystemBytes）。
 * - 本分配器仅保证“本端发出的消息”在 in_use
 * 集合中的唯一性；不尝试与对端全局去重。
 * - 默认假设在同一执行器/线程语境下使用；若跨线程使用，本类内部使用 mutex
 * 保护数据结构。
 */
class SystemBytes final {
public:
    explicit SystemBytes(std::uint32_t initial = 1U) noexcept;

    /**
     * @brief 分配一个新的 SystemBytes。
     *
     * @return ok 成功；buffer_overflow 表示可用空间被耗尽（极端情况：2^32-1
     * 全部在用）。
     */
    [[nodiscard]] std::error_code allocate(std::uint32_t &out) noexcept;

    /**
     * @brief 释放一个 SystemBytes（允许重复 release，多次调用会被忽略）。
     */
    void release(std::uint32_t system_bytes) noexcept;

    [[nodiscard]] bool is_in_use(std::uint32_t system_bytes) const noexcept;
    [[nodiscard]] std::size_t in_use_count() const noexcept;

private:
    [[nodiscard]] std::uint32_t next_candidate_() noexcept;

    mutable std::mutex mu_{};
    std::uint32_t next_{1U};
    std::deque<std::uint32_t> free_{};
    std::unordered_set<std::uint32_t> in_use_{};
};

} // namespace secs::protocol
