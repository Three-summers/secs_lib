#pragma once

#include "secs/ii/item.hpp"

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
};

const std::error_category& error_category() noexcept;
std::error_code make_error_code(errc e) noexcept;

/**
 * @brief 计算 Item 编码后的字节数（含头部与 payload）。
 */
std::error_code encoded_size(const Item& item, std::size_t& out_size) noexcept;

/**
 * @brief 编码 Item 并追加到 out（内部会一次性 reserve/resize，避免反复 realloc）。
 */
std::error_code encode(const Item& item, std::vector<byte>& out) noexcept;

/**
 * @brief 编码 Item 到固定缓冲区（用于零拷贝/流式写入场景）。
 *
 * 注意：
 * - out 过小会返回 errc::buffer_overflow
 * - 成功时 written 为写入字节数
 */
std::error_code encode_to(mutable_bytes_view out, const Item& item, std::size_t& written) noexcept;

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
std::error_code decode_one(bytes_view in, Item& out, std::size_t& consumed) noexcept;

}  // namespace secs::ii

namespace std {
template <>
struct is_error_code_enum<secs::ii::errc> : true_type {};
}  // namespace std

