#pragma once

#include "secs/core/common.hpp"
#include "secs/core/error.hpp"

#include <array>
#include <cstddef>
#include <memory>
#include <system_error>

namespace secs::core {

/**
 * @brief 预分配 + 可扩容的字节缓冲区（读写指针模型）。
 *
 * 设计目标：
 * - 小包场景优先走 inline 预分配（默认 8KB）
 * - 支持 compact（搬移可读区到头部）减少扩容次数
 * - 必要时 grow（扩容到 heap），并保持已有数据
 *
 * 注意：
 * - 本类不做线程安全保证。
 */
class FixedBuffer final {
public:
    explicit FixedBuffer(
        std::size_t initial_capacity = kDefaultFixedBufferCapacity,
        std::size_t max_capacity = kDefaultFixedBufferMaxCapacity);

    FixedBuffer(FixedBuffer &&other) noexcept;
    FixedBuffer &operator=(FixedBuffer &&other) noexcept;

    FixedBuffer(const FixedBuffer &) = delete;
    FixedBuffer &operator=(const FixedBuffer &) = delete;

    ~FixedBuffer() = default;

    [[nodiscard]] std::size_t capacity() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool empty() const noexcept;

    void clear() noexcept;
    void compact() noexcept;

    [[nodiscard]] bytes_view readable_bytes() const noexcept;
    [[nodiscard]] mutable_bytes_view writable_bytes() noexcept;

    std::error_code commit(std::size_t n) noexcept;
    std::error_code append(bytes_view data) noexcept;
    std::error_code consume(std::size_t n) noexcept;
    std::error_code reserve(std::size_t new_capacity) noexcept;

private:
    [[nodiscard]] byte *data_mutable() noexcept;
    [[nodiscard]] const byte *data_const() const noexcept;

    std::error_code ensure_writable(std::size_t n) noexcept;
    std::error_code grow(std::size_t min_capacity) noexcept;

    std::array<byte, kDefaultFixedBufferCapacity> inline_{};
    std::unique_ptr<byte[]> heap_;

    std::size_t max_capacity_{0};
    std::size_t capacity_{0};
    std::size_t read_pos_{0};
    std::size_t write_pos_{0};
};

} // namespace secs::core
