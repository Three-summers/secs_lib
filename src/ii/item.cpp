#include "secs/ii/item.hpp"

#include <bit>
#include <type_traits>

namespace secs::ii {
namespace {

// 浮点比较采用“按位相等”而不是“容差比较”：
// - SECS-II 编解码以字节为单位，关注的是位模式是否一致
// - 这样可以正确处理 NaN、-0/+0 等边界情况（按值比较可能产生歧义）
bool float_bits_equal(float a, float b) noexcept {
  return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b);
}

bool double_bits_equal(double a, double b) noexcept {
  return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
}

bool f4_equal(const F4& lhs, const F4& rhs) noexcept {
  if (lhs.values.size() != rhs.values.size()) {
    return false;
  }
  for (std::size_t i = 0; i < lhs.values.size(); ++i) {
    if (!float_bits_equal(lhs.values[i], rhs.values[i])) {
      return false;
    }
  }
  return true;
}

bool f8_equal(const F8& lhs, const F8& rhs) noexcept {
  if (lhs.values.size() != rhs.values.size()) {
    return false;
  }
  for (std::size_t i = 0; i < lhs.values.size(); ++i) {
    if (!double_bits_equal(lhs.values[i], rhs.values[i])) {
      return false;
    }
  }
  return true;
}

}  // namespace

Item::Item(List v) : storage_(std::move(v)) {}
Item::Item(ASCII v) : storage_(std::move(v)) {}
Item::Item(Binary v) : storage_(std::move(v)) {}
Item::Item(Boolean v) : storage_(std::move(v)) {}
Item::Item(I1 v) : storage_(std::move(v)) {}
Item::Item(I2 v) : storage_(std::move(v)) {}
Item::Item(I4 v) : storage_(std::move(v)) {}
Item::Item(I8 v) : storage_(std::move(v)) {}
Item::Item(U1 v) : storage_(std::move(v)) {}
Item::Item(U2 v) : storage_(std::move(v)) {}
Item::Item(U4 v) : storage_(std::move(v)) {}
Item::Item(U8 v) : storage_(std::move(v)) {}
Item::Item(F4 v) : storage_(std::move(v)) {}
Item::Item(F8 v) : storage_(std::move(v)) {}

Item Item::list(std::vector<Item> values) {
  return Item(List{std::move(values)});
}

Item Item::ascii(std::string value) {
  return Item(ASCII{std::move(value)});
}

Item Item::binary(std::vector<byte> value) {
  return Item(Binary{std::move(value)});
}

Item Item::boolean(std::vector<bool> values) {
  return Item(Boolean{std::move(values)});
}

Item Item::i1(std::vector<std::int8_t> values) {
  return Item(I1{std::move(values)});
}
Item Item::i2(std::vector<std::int16_t> values) {
  return Item(I2{std::move(values)});
}
Item Item::i4(std::vector<std::int32_t> values) {
  return Item(I4{std::move(values)});
}
Item Item::i8(std::vector<std::int64_t> values) {
  return Item(I8{std::move(values)});
}

Item Item::u1(std::vector<std::uint8_t> values) {
  return Item(U1{std::move(values)});
}
Item Item::u2(std::vector<std::uint16_t> values) {
  return Item(U2{std::move(values)});
}
Item Item::u4(std::vector<std::uint32_t> values) {
  return Item(U4{std::move(values)});
}
Item Item::u8(std::vector<std::uint64_t> values) {
  return Item(U8{std::move(values)});
}

Item Item::f4(std::vector<float> values) {
  return Item(F4{std::move(values)});
}
Item Item::f8(std::vector<double> values) {
  return Item(F8{std::move(values)});
}

bool operator==(const Item& lhs, const Item& rhs) noexcept {
  if (lhs.storage_.index() != rhs.storage_.index()) {
    return false;
  }
  return std::visit(
    [&](const auto& a) -> bool {
      using T = std::decay_t<decltype(a)>;
      const auto* b = std::get_if<T>(&rhs.storage_);
      if (!b) {
        return false;
      }
      if constexpr (std::is_same_v<T, F4>) {
        return f4_equal(a, *b);
      } else if constexpr (std::is_same_v<T, F8>) {
        return f8_equal(a, *b);
      } else {
        return a == *b;
      }
    },
    lhs.storage_);
}

}  // namespace secs::ii
