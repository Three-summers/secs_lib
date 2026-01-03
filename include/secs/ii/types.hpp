#pragma once

#include "secs/core/common.hpp"

#include <cstddef>
#include <cstdint>

namespace secs::ii {

using byte = secs::core::byte;
using bytes_view = secs::core::bytes_view;
using mutable_bytes_view = secs::core::mutable_bytes_view;

/**
 * @brief SECS-II 数据项的 6 位格式码（SEMI E5）。
 *
 * 编码时会写入一个“格式字节”：
 * - 高 6 位：format_code
 * - 低 2 位：长度字段字节数（01->1B, 10->2B, 11->3B）
 */
enum class format_code : std::uint8_t {
    list = 0x00,
    binary = 0x08,
    boolean = 0x09,
    ascii = 0x10,

    i8 = 0x18,
    i1 = 0x19,
    i2 = 0x1A,
    i4 = 0x1C,

    f8 = 0x20,
    f4 = 0x24,

    u8 = 0x28,
    u1 = 0x29,
    u2 = 0x2A,
    u4 = 0x2C,
};

inline constexpr std::size_t kMaxLength = 0x00FF'FFFFu; // 3 字节长度字段最大值

} // namespace secs::ii
