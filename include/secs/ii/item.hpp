#pragma once

#include "secs/ii/types.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace secs::ii {

class Item;
using List = std::vector<Item>;

struct ASCII final {
  std::string value;
  friend bool operator==(const ASCII&, const ASCII&) = default;
};

struct Binary final {
  std::vector<byte> value;
  friend bool operator==(const Binary&, const Binary&) = default;
};

struct Boolean final {
  std::vector<bool> values;
  friend bool operator==(const Boolean&, const Boolean&) = default;
};

struct I1 final {
  std::vector<std::int8_t> values;
  friend bool operator==(const I1&, const I1&) = default;
};
struct I2 final {
  std::vector<std::int16_t> values;
  friend bool operator==(const I2&, const I2&) = default;
};
struct I4 final {
  std::vector<std::int32_t> values;
  friend bool operator==(const I4&, const I4&) = default;
};
struct I8 final {
  std::vector<std::int64_t> values;
  friend bool operator==(const I8&, const I8&) = default;
};

struct U1 final {
  std::vector<std::uint8_t> values;
  friend bool operator==(const U1&, const U1&) = default;
};
struct U2 final {
  std::vector<std::uint16_t> values;
  friend bool operator==(const U2&, const U2&) = default;
};
struct U4 final {
  std::vector<std::uint32_t> values;
  friend bool operator==(const U4&, const U4&) = default;
};
struct U8 final {
  std::vector<std::uint64_t> values;
  friend bool operator==(const U8&, const U8&) = default;
};

struct F4 final {
  std::vector<float> values;
  friend bool operator==(const F4&, const F4&) = default;
};
struct F8 final {
  std::vector<double> values;
  friend bool operator==(const F8&, const F8&) = default;
};

/**
 * @brief SECS-II 数据项（强类型，支持嵌套 List）。
 *
 * 约定：
 * - 数值类与 Boolean 都使用“向量”承载（支持 0..N 个元素）。
 * - ASCII 使用 std::string（允许包含 '\\0'，以字节序列视角编码）。
 */
class Item final {
 public:
  using storage_type = std::variant<List, ASCII, Binary, Boolean, I1, I2, I4, I8, U1, U2, U4, U8, F4, F8>;

  Item() = delete;

  explicit Item(List v);
  explicit Item(ASCII v);
  explicit Item(Binary v);
  explicit Item(Boolean v);
  explicit Item(I1 v);
  explicit Item(I2 v);
  explicit Item(I4 v);
  explicit Item(I8 v);
  explicit Item(U1 v);
  explicit Item(U2 v);
  explicit Item(U4 v);
  explicit Item(U8 v);
  explicit Item(F4 v);
  explicit Item(F8 v);

  [[nodiscard]] const storage_type& storage() const noexcept { return storage_; }
  [[nodiscard]] storage_type& storage() noexcept { return storage_; }

  template <class T>
  [[nodiscard]] const T* get_if() const noexcept {
    return std::get_if<T>(&storage_);
  }

  template <class T>
  [[nodiscard]] T* get_if() noexcept {
    return std::get_if<T>(&storage_);
  }

  [[nodiscard]] bool is_list() const noexcept { return std::holds_alternative<List>(storage_); }

  static Item list(std::vector<Item> values);
  static Item ascii(std::string value);
  static Item binary(std::vector<byte> value);
  static Item boolean(std::vector<bool> values);

  static Item i1(std::vector<std::int8_t> values);
  static Item i2(std::vector<std::int16_t> values);
  static Item i4(std::vector<std::int32_t> values);
  static Item i8(std::vector<std::int64_t> values);

  static Item u1(std::vector<std::uint8_t> values);
  static Item u2(std::vector<std::uint16_t> values);
  static Item u4(std::vector<std::uint32_t> values);
  static Item u8(std::vector<std::uint64_t> values);

  static Item f4(std::vector<float> values);
  static Item f8(std::vector<double> values);

  friend bool operator==(const Item& lhs, const Item& rhs) noexcept;
  friend bool operator!=(const Item& lhs, const Item& rhs) noexcept { return !(lhs == rhs); }

 private:
  storage_type storage_;
};

}  // namespace secs::ii
