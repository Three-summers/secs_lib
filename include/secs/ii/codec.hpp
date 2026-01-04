#pragma once

#include "secs/ii/item.hpp"

#include <cstddef>
#include <cstdint>
#include <system_error>
#include <vector>

namespace secs::ii {

enum class errc : int {
    ok = 0,
    truncated = 1,
    invalid_header = 2,
    invalid_format = 3,
    length_overflow = 4,
    length_mismatch = 5,
    buffer_overflow = 6,
    list_too_large = 7,
    payload_too_large = 8,
    total_budget_exceeded = 9,
    out_of_memory = 10,
};

const std::error_category &error_category() noexcept;
std::error_code make_error_code(errc e) noexcept;

/**
 * @brief 解码资源限制（用于约束不可信输入的资源消耗）。
 *
 * 说明：
 * - 该限制仅作用于 decode_one（解码方向），编码不受影响。
 * - 默认值为“较宽松但有上界”的设置；上层可按互通对象能力（例如 max nested
 * level=16）收紧。
 */
struct DecodeLimits final {
    // 允许的最大嵌套深度：depth 从 0 开始，只有 depth > max_depth 才拒绝。
    std::size_t max_depth{64};

    // List 的 length 语义是“子元素个数”（不是字节数），这里做硬上限防止超大
    // reserve/循环。
    std::uint32_t max_list_items{65'535u};

    // 单个 Item 的 payload 最大字节数（例如 Binary/ASCII）。
    std::uint32_t max_payload_bytes{4u * 1024u * 1024u}; // 4MB

    // 整棵树的最大节点数（含 List 节点与叶子节点）。
    std::size_t max_total_items{1u * 1024u * 1024u}; // 1,048,576

    // 整棵树的 payload 总字节数上限（仅累计非 List 的 payload bytes）。
    std::size_t max_total_bytes{64u * 1024u * 1024u}; // 64MB
};

/**
 * @brief 计算 Item 编码后的字节数（含头部与 payload）。
 */
std::error_code encoded_size(const Item &item, std::size_t &out_size) noexcept;

/**
 * @brief 编码 Item 并追加到 out（内部会一次性 reserve/resize，避免反复
 * realloc）。
 */
std::error_code encode(const Item &item, std::vector<byte> &out) noexcept;

/**
 * @brief 编码 Item 到固定缓冲区（用于零拷贝/流式写入场景）。
 *
 * 注意：
 * - out 过小会返回 errc::buffer_overflow
 * - 成功时 written 为写入字节数
 */
std::error_code encode_to(mutable_bytes_view out,
                          const Item &item,
                          std::size_t &written) noexcept;

/**
 * @brief 从输入缓冲区解码一个 Item（流式 API）。
 *
 * 成功时：
 * - out 被填充
 * - consumed 为消耗的输入字节数（可用于从流中“吃掉”已解析部分）
 *
 * 失败时：
 * - 返回非零 error_code（截断/非法格式/长度不匹配等）
 */
std::error_code
decode_one(bytes_view in, Item &out, std::size_t &consumed) noexcept;

/**
 * @brief 从输入缓冲区解码一个 Item（带资源限制）。
 */
std::error_code decode_one(bytes_view in,
                           Item &out,
                           std::size_t &consumed,
                           const DecodeLimits &limits) noexcept;

} // namespace secs::ii

namespace std {
template <>
struct is_error_code_enum<secs::ii::errc> : true_type {};
} // namespace std
