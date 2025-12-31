#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <chrono>

namespace secs::core {

using byte = std::uint8_t;
using bytes_view = std::span<const byte>;
using mutable_bytes_view = std::span<byte>;

inline constexpr std::size_t kDefaultFixedBufferCapacity = 8 * 1024;
// FixedBuffer 默认初始容量：小包优先走预分配路径，减少频繁堆分配。
//
// FixedBuffer 默认最大容量：用于避免极端情况下无上限扩容导致内存耗尽。
inline constexpr std::size_t kDefaultFixedBufferMaxCapacity = 64 * 1024 * 1024;  // 64MB

using steady_clock = std::chrono::steady_clock;
using duration = steady_clock::duration;

}  // 命名空间 secs::core
