#include "secs/core/buffer.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

namespace secs::core {
namespace {

std::size_t clamp_capacity(std::size_t requested,
                           std::size_t max_capacity) noexcept {
    if (requested == 0 || max_capacity == 0) {
        return 0;
    }
    return std::min(requested, max_capacity);
}

} // namespace

/*
 * FixedBuffer 的实现模型：
 * - 读写指针：read_pos_ / write_pos_ 表示“可读区间”。
 * - 小包优先走 inline_（固定数组）；必要时切换到 heap_ 并按需扩容。
 * - ensure_writable(n) 会按顺序尝试：
 *   1) 当前尾部空间是否足够
 *   2) compact() 把可读数据搬到头部，回收前缀空洞
 *   3) grow() 扩容（按 2 倍增长，且受 max_capacity_ 上限约束）
 */
FixedBuffer::FixedBuffer(std::size_t initial_capacity, std::size_t max_capacity)
    : max_capacity_(max_capacity),
      capacity_(clamp_capacity(initial_capacity, max_capacity_)) {
    if (capacity_ > inline_.size()) {
        heap_ = std::make_unique<byte[]>(capacity_);
    }
}

FixedBuffer::FixedBuffer(FixedBuffer &&other) noexcept
    : heap_(std::move(other.heap_)), max_capacity_(other.max_capacity_),
      capacity_(other.capacity_), read_pos_(other.read_pos_),
      write_pos_(other.write_pos_) {
    // 优化：避免在 heap_ 场景下搬运 8KB inline_；仅在 inline_ 场景复制“实际用到”的字节。
    if (!heap_ && write_pos_ > read_pos_) {
        std::memcpy(inline_.data() + read_pos_,
                    other.inline_.data() + read_pos_,
                    write_pos_ - read_pos_);
    }
    other.max_capacity_ = 0;
    other.capacity_ = 0;
    other.read_pos_ = 0;
    other.write_pos_ = 0;
}

FixedBuffer &FixedBuffer::operator=(FixedBuffer &&other) noexcept {
    if (this == &other) {
        return *this;
    }
    heap_ = std::move(other.heap_);
    max_capacity_ = other.max_capacity_;
    capacity_ = other.capacity_;
    read_pos_ = other.read_pos_;
    write_pos_ = other.write_pos_;

    if (!heap_ && write_pos_ > read_pos_) {
        std::memcpy(inline_.data() + read_pos_,
                    other.inline_.data() + read_pos_,
                    write_pos_ - read_pos_);
    }

    other.max_capacity_ = 0;
    other.capacity_ = 0;
    other.read_pos_ = 0;
    other.write_pos_ = 0;
    return *this;
}

std::size_t FixedBuffer::capacity() const noexcept { return capacity_; }

std::size_t FixedBuffer::size() const noexcept {
    return write_pos_ - read_pos_;
}

bool FixedBuffer::empty() const noexcept { return size() == 0; }

byte *FixedBuffer::data_mutable() noexcept {
    if (heap_) {
        return heap_.get();
    }
    return inline_.data();
}

const byte *FixedBuffer::data_const() const noexcept {
    if (heap_) {
        return heap_.get();
    }
    return inline_.data();
}

void FixedBuffer::clear() noexcept {
    read_pos_ = 0;
    write_pos_ = 0;
}

void FixedBuffer::compact() noexcept {
    const auto readable = size();
    if (readable == 0) {
        clear();
        return;
    }
    if (read_pos_ == 0) {
        return;
    }
    // 将可读区整体搬到头部，保持数据连续，减少后续扩容次数。
    std::memmove(data_mutable(), data_const() + read_pos_, readable);
    read_pos_ = 0;
    write_pos_ = readable;
}

bytes_view FixedBuffer::readable_bytes() const noexcept {
    return bytes_view{data_const() + read_pos_, size()};
}

mutable_bytes_view FixedBuffer::writable_bytes() noexcept {
    if (write_pos_ > capacity_) {
        return {};
    }
    return mutable_bytes_view{data_mutable() + write_pos_,
                              capacity_ - write_pos_};
}

std::error_code FixedBuffer::commit(std::size_t n) noexcept {
    if (n == 0) {
        return {};
    }
    if (write_pos_ > capacity_ || n > (capacity_ - write_pos_)) {
        return make_error_code(errc::invalid_argument);
    }
    write_pos_ += n;
    return {};
}

std::error_code FixedBuffer::consume(std::size_t n) noexcept {
    if (n == 0) {
        return {};
    }
    if (n > size()) {
        return make_error_code(errc::invalid_argument);
    }
    read_pos_ += n;
    if (read_pos_ == write_pos_) {
        clear();
    }
    return {};
}

std::error_code FixedBuffer::reserve(std::size_t new_capacity) noexcept {
    if (new_capacity <= capacity_) {
        return {};
    }
    if (new_capacity > max_capacity_) {
        return make_error_code(errc::buffer_overflow);
    }
    return grow(new_capacity);
}

std::error_code FixedBuffer::ensure_writable(std::size_t n) noexcept {
    if (n == 0) {
        return {};
    }
    if (write_pos_ > capacity_) {
        return make_error_code(errc::invalid_argument);
    }
    // 代表有足够空间，返回 {} 即可
    if (capacity_ - write_pos_ >= n) {
        return {};
    }

    if (read_pos_ != 0) {
        // 尾部空间不够时，先尝试 compact 回收前缀空洞。
        compact();
        if (write_pos_ > capacity_) {
            return make_error_code(errc::invalid_argument);
        }
        if (capacity_ - write_pos_ >= n) {
            return {};
        }
    }

    const auto readable = size();
    if (n > (std::numeric_limits<std::size_t>::max() - readable)) {
        return make_error_code(errc::buffer_overflow);
    }
    const auto required = readable + n;
    if (required > max_capacity_) {
        return make_error_code(errc::buffer_overflow);
    }
    return grow(required);
}

std::error_code FixedBuffer::grow(std::size_t min_capacity) noexcept {
    if (min_capacity <= capacity_) {
        return {};
    }
    if (min_capacity > max_capacity_) {
        return make_error_code(errc::buffer_overflow);
    }

    if (!heap_ && min_capacity <= inline_.size()) {
        // 仍可容纳在 inline_ 中：只更新可用容量，不做堆分配。
        capacity_ = min_capacity;
        return {};
    }

    // 扩容策略：按 2 倍增长，直到 >= min_capacity，且不超过 max_capacity_。
    // 这里的如果 capacity_ 为 0，则赋值成 1 是为了防止扩容操作无效
    std::size_t new_capacity =
        std::max<std::size_t>(capacity_ == 0 ? 1 : capacity_, 1);
    while (new_capacity < min_capacity) {
        if (new_capacity > (std::numeric_limits<std::size_t>::max() / 2)) {
            return make_error_code(errc::buffer_overflow);
        }
        new_capacity *= 2;
        new_capacity = std::min(new_capacity, max_capacity_);
    }

    const auto readable = size();
    auto new_heap = std::make_unique<byte[]>(new_capacity);
    if (readable != 0) {
        std::memcpy(new_heap.get(), data_const() + read_pos_, readable);
    }
    heap_ = std::move(new_heap);
    capacity_ = new_capacity;
    read_pos_ = 0;
    write_pos_ = readable;
    return {};
}

std::error_code FixedBuffer::append(bytes_view data) noexcept {
    if (data.empty()) {
        return {};
    }
    auto ec = ensure_writable(data.size());
    // std::error_code 重载了 bool 运算符，当是 {} 是为 false
    if (ec) {
        return ec;
    }
    std::memcpy(data_mutable() + write_pos_, data.data(), data.size());
    write_pos_ += data.size();
    return {};
}

} // namespace secs::core
