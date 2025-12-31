#include "secs/core/buffer.hpp"
#include "secs/core/error.hpp"

#include "test_main.hpp"

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace {

using secs::core::FixedBuffer;
using secs::core::byte;
using secs::core::bytes_view;
using secs::core::errc;
using secs::core::make_error_code;

bytes_view as_bytes(std::string_view s) {
  return bytes_view{reinterpret_cast<const byte*>(s.data()), s.size()};
}

void test_append_consume_basic() {
  FixedBuffer buf(16);
  TEST_EXPECT(buf.empty());
  TEST_EXPECT_EQ(buf.size(), 0u);

  TEST_EXPECT_OK(buf.append(as_bytes("hello")));
  TEST_EXPECT_EQ(buf.size(), 5u);
  TEST_EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(buf.readable_bytes().data()),
                                  buf.readable_bytes().size()),
                 "hello");

  TEST_EXPECT_OK(buf.consume(2));
  TEST_EXPECT_EQ(buf.size(), 3u);

  TEST_EXPECT_OK(buf.append(as_bytes("!")));
  TEST_EXPECT_EQ(buf.size(), 4u);

  TEST_EXPECT_OK(buf.consume(4));
  TEST_EXPECT(buf.empty());
}

void test_compact() {
  FixedBuffer buf(8);
  TEST_EXPECT_OK(buf.append(as_bytes("abcdef")));
  TEST_EXPECT_OK(buf.consume(4));  // 剩下 "ef"

  TEST_EXPECT_OK(buf.append(as_bytes("WXYZ")));
  auto out = buf.readable_bytes();
  TEST_EXPECT_EQ(out.size(), 6u);
  TEST_EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(out.data()), out.size()), "efWXYZ");
}

void test_grow_preserve_data() {
  FixedBuffer buf(8);
  TEST_EXPECT_OK(buf.append(as_bytes("12345678")));
  TEST_EXPECT_OK(buf.append(as_bytes("9")));  // 触发扩容（grow）

  auto out = buf.readable_bytes();
  TEST_EXPECT_EQ(out.size(), 9u);
  TEST_EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(out.data()), out.size()), "123456789");
  TEST_EXPECT(buf.capacity() >= 9u);
}

void test_commit_api() {
  FixedBuffer buf(8);
  auto w = buf.writable_bytes();
  TEST_EXPECT(w.size() >= 4u);
  w[0] = static_cast<byte>('t');
  w[1] = static_cast<byte>('e');
  w[2] = static_cast<byte>('s');
  w[3] = static_cast<byte>('t');
  TEST_EXPECT_OK(buf.commit(4));
  TEST_EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(buf.readable_bytes().data()),
                                  buf.readable_bytes().size()),
                 "test");
}

void test_bounds() {
  FixedBuffer buf(4);
  TEST_EXPECT_OK(buf.append(as_bytes("abcd")));

  auto ec = buf.consume(5);
  TEST_EXPECT_EQ(ec, make_error_code(errc::invalid_argument));

  ec = buf.commit(1);
  TEST_EXPECT_EQ(ec, make_error_code(errc::invalid_argument));
}

void test_move_semantics() {
  FixedBuffer buf(8);
  TEST_EXPECT_OK(buf.append(as_bytes("move")));

  FixedBuffer moved(std::move(buf));
  TEST_EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(moved.readable_bytes().data()),
                                  moved.readable_bytes().size()),
                 "move");

  FixedBuffer assigned(1);
  assigned = std::move(moved);
  TEST_EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(assigned.readable_bytes().data()),
                                  assigned.readable_bytes().size()),
                 "move");
}

void test_zero_capacity() {
  FixedBuffer buf(0);
  TEST_EXPECT_EQ(buf.capacity(), 0u);
  TEST_EXPECT(buf.empty());
}

void test_large_heap_buffer() {
  FixedBuffer buf(16 * 1024);
  TEST_EXPECT_OK(buf.append(as_bytes("large buffer test")));
  TEST_EXPECT_EQ(buf.size(), 17u);

  auto readable = buf.readable_bytes();
  TEST_EXPECT(!readable.empty());

  auto writable = buf.writable_bytes();
  TEST_EXPECT(!writable.empty());
}

void test_self_move_assignment() {
  FixedBuffer buf(8);
  TEST_EXPECT_OK(buf.append(as_bytes("self")));
  buf = std::move(buf);
  TEST_EXPECT_EQ(buf.size(), 4u);
}

void test_compact_edge_cases() {
  FixedBuffer buf(8);
  buf.compact();
  TEST_EXPECT(buf.empty());

  TEST_EXPECT_OK(buf.append(as_bytes("test")));
  buf.compact();
  TEST_EXPECT_EQ(buf.size(), 4u);
}

void test_writable_overflow() {
  FixedBuffer buf(4);
  TEST_EXPECT_OK(buf.commit(4));
  auto w = buf.writable_bytes();
  TEST_EXPECT(w.empty());
}

void test_zero_operations() {
  FixedBuffer buf(8);
  TEST_EXPECT_OK(buf.commit(0));
  TEST_EXPECT_OK(buf.consume(0));
  TEST_EXPECT_OK(buf.append(bytes_view{}));
}

void test_reserve() {
  FixedBuffer buf(4);
  TEST_EXPECT_OK(buf.reserve(2));
  TEST_EXPECT_EQ(buf.capacity(), 4u);
  TEST_EXPECT_OK(buf.reserve(8));
  TEST_EXPECT(buf.capacity() >= 8u);
}

void test_max_capacity_reserve_rejects() {
  FixedBuffer buf(4, 8);
  auto ec = buf.reserve(9);
  TEST_EXPECT_EQ(ec, make_error_code(errc::buffer_overflow));

  ec = buf.reserve(std::numeric_limits<std::size_t>::max());
  TEST_EXPECT_EQ(ec, make_error_code(errc::buffer_overflow));
}

void test_max_capacity_append_rejects() {
  FixedBuffer buf(4, 8);
  TEST_EXPECT_OK(buf.append(as_bytes("1234")));
  auto ec = buf.append(as_bytes("56789"));  // total=9 > max=8
  TEST_EXPECT_EQ(ec, make_error_code(errc::buffer_overflow));
}

void test_max_capacity_zero_rejects_growth() {
  FixedBuffer buf(16, 0);
  TEST_EXPECT_EQ(buf.capacity(), 0u);
  auto ec = buf.append(as_bytes("a"));
  TEST_EXPECT_EQ(ec, make_error_code(errc::buffer_overflow));
}

void test_default_max_capacity_reserve_boundaries() {
  FixedBuffer buf;

  TEST_EXPECT_OK(buf.reserve(secs::core::kDefaultFixedBufferMaxCapacity));
  TEST_EXPECT_EQ(buf.capacity(), secs::core::kDefaultFixedBufferMaxCapacity);

  auto ec = buf.reserve(secs::core::kDefaultFixedBufferMaxCapacity + 1);
  TEST_EXPECT_EQ(ec, make_error_code(errc::buffer_overflow));
}

void test_custom_max_capacity_reserve_boundaries() {
  constexpr std::size_t kOneMiB = 1024 * 1024;
  FixedBuffer buf(8 * 1024, kOneMiB);

  auto ec = buf.reserve(kOneMiB + 1);
  TEST_EXPECT_EQ(ec, make_error_code(errc::buffer_overflow));

  ec = buf.reserve(2 * kOneMiB);
  TEST_EXPECT_EQ(ec, make_error_code(errc::buffer_overflow));
}

void test_size_t_addition_overflow_is_rejected() {
  FixedBuffer buf(16, std::numeric_limits<std::size_t>::max());
  TEST_EXPECT_OK(buf.append(as_bytes("x")));

  byte dummy = 0;
  bytes_view huge{&dummy, std::numeric_limits<std::size_t>::max()};
  auto ec = buf.append(huge);
  TEST_EXPECT_EQ(ec, make_error_code(errc::buffer_overflow));
  TEST_EXPECT_EQ(buf.size(), 1u);
}

void test_compact_then_grow() {
  FixedBuffer buf(8);
  TEST_EXPECT_OK(buf.append(as_bytes("12345678")));
  TEST_EXPECT_OK(buf.consume(4));  // 剩下 "5678"

  TEST_EXPECT_OK(buf.append(as_bytes("abcde")));  // compact（整理）后仍不足，触发扩容
  auto out = buf.readable_bytes();
  TEST_EXPECT_EQ(out.size(), 9u);
  TEST_EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(out.data()), out.size()), "5678abcde");
}

void test_reserve_from_zero_to_heap() {
  FixedBuffer buf(0);
  TEST_EXPECT_OK(buf.reserve(16 * 1024));
  TEST_EXPECT(buf.capacity() >= 16 * 1024u);
}

void test_reserve_size_max_hits_overflow_guard() {
  FixedBuffer buf(1, std::numeric_limits<std::size_t>::max());
  auto ec = buf.reserve(std::numeric_limits<std::size_t>::max());
  TEST_EXPECT_EQ(ec, make_error_code(errc::buffer_overflow));
  TEST_EXPECT_EQ(buf.capacity(), 1u);
}

void test_heap_grow_preserves_data() {
  FixedBuffer buf(secs::core::kDefaultFixedBufferCapacity);
  std::string payload(secs::core::kDefaultFixedBufferCapacity, 'a');
  TEST_EXPECT_OK(buf.append(as_bytes(payload)));
  TEST_EXPECT_EQ(buf.size(), secs::core::kDefaultFixedBufferCapacity);

  TEST_EXPECT_OK(buf.append(as_bytes("b")));  // 触发从内联缓冲区到堆缓冲区的扩容
  TEST_EXPECT_EQ(buf.size(), secs::core::kDefaultFixedBufferCapacity + 1);

  auto out = buf.readable_bytes();
  TEST_EXPECT_EQ(out.size(), secs::core::kDefaultFixedBufferCapacity + 1);
  TEST_EXPECT_EQ(static_cast<char>(out.front()), 'a');
  TEST_EXPECT_EQ(static_cast<char>(out.back()), 'b');
  TEST_EXPECT(buf.capacity() > secs::core::kDefaultFixedBufferCapacity);
}

void test_grow_with_zero_capacity() {
  FixedBuffer buf(0);
  TEST_EXPECT_OK(buf.append(as_bytes("grow")));
  TEST_EXPECT(buf.capacity() > 0u);
}

void test_append_failure() {
  FixedBuffer buf(4);
  TEST_EXPECT_OK(buf.append(as_bytes("1234")));
  auto ec = buf.append(as_bytes("overflow"));
  if (!ec) {
    TEST_EXPECT(buf.capacity() > 4u);
  }
}

void test_data_accessors() {
  FixedBuffer buf(16 * 1024);
  TEST_EXPECT_OK(buf.append(as_bytes("heap")));
  auto readable = buf.readable_bytes();
  TEST_EXPECT(!readable.empty());
}

void test_grow_from_empty_heap() {
  FixedBuffer buf(16 * 1024);
  TEST_EXPECT_OK(buf.reserve(32 * 1024));
  TEST_EXPECT(buf.capacity() >= 32 * 1024u);
}

}  // 匿名命名空间

int main() {
  test_append_consume_basic();
  test_compact();
  test_grow_preserve_data();
  test_commit_api();
  test_bounds();
  test_move_semantics();
  test_zero_capacity();
  test_large_heap_buffer();
  test_self_move_assignment();
  test_compact_edge_cases();
  test_writable_overflow();
  test_zero_operations();
  test_reserve();
  test_max_capacity_reserve_rejects();
  test_max_capacity_append_rejects();
  test_max_capacity_zero_rejects_growth();
  test_default_max_capacity_reserve_boundaries();
  test_custom_max_capacity_reserve_boundaries();
  test_size_t_addition_overflow_is_rejected();
  test_compact_then_grow();
  test_reserve_from_zero_to_heap();
  test_reserve_size_max_hits_overflow_guard();
  test_heap_grow_preserves_data();
  test_grow_with_zero_capacity();
  test_append_failure();
  test_data_accessors();
  test_grow_from_empty_heap();
  return ::secs::tests::run_and_report();
}
