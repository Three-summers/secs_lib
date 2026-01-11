#pragma once

#include "secs/core/common.hpp"
#include "secs/ii/codec.hpp"
#include "secs/ii/item.hpp"

#include <cstddef>
#include <optional>
#include <system_error>
#include <utility>
#include <vector>

namespace secs::utils {

/**
 * @brief SECS-II Item 编解码的轻量包装：减少样板代码。
 *
 * 说明：
 * - 该文件只做“薄封装”，不改变底层编解码语义；
 * - decode 方向支持 DecodeLimits（用于限制不可信输入的资源消耗）。
 */

struct DecodeOneItemResult final {
    // 解码得到的 Item（成功时有效）。
    secs::ii::Item item{secs::ii::List{}};

    // decode_one 消耗的输入字节数（成功时有意义）。
    std::size_t consumed{0};

    // 是否完整消耗输入缓冲区（consumed == in.size()）。
    bool fully_consumed{false};
};

/**
 * @brief 编码 Item，返回 {ec, bytes}。
 */
[[nodiscard]] std::pair<std::error_code, std::vector<secs::core::byte>>
encode_item(const secs::ii::Item &item) noexcept;

/**
 * @brief 解码一个 Item，返回 {ec, result}。
 *
 * - 成功时 result.item 为解码后的 Item；result.consumed 为消耗字节数；
 * - result.fully_consumed 可用于判断“是否只有一个 Item，没有尾随垃圾字节”。
 */
[[nodiscard]] std::pair<std::error_code, DecodeOneItemResult>
decode_one_item(secs::core::bytes_view in,
                const secs::ii::DecodeLimits &limits = {}) noexcept;

/**
 * @brief 若输入为空则返回 {ok, nullopt}；否则等价于 decode_one_item。
 *
 * 典型用于处理“协议层 body 允许为空”的场景，避免用户侧反复写 if (empty)。
 */
[[nodiscard]] std::pair<std::error_code, std::optional<DecodeOneItemResult>>
decode_one_item_if_any(secs::core::bytes_view in,
                       const secs::ii::DecodeLimits &limits = {}) noexcept;

} // namespace secs::utils

