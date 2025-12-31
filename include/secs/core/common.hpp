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
// FixedBuffer 默认最大容量：用于避免极端情况下无上限扩容导致 OOM。
inline constexpr std::size_t kDefaultFixedBufferMaxCapacity = 64 * 1024 * 1024;  // 64MB

using steady_clock = std::chrono::steady_clock;
using duration = steady_clock::duration;

}  // namespace secs::core
