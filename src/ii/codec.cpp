#include "secs/ii/codec.hpp"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <type_traits>

namespace secs::ii {
namespace {

class secs_ii_error_category final : public std::error_category {
 public:
  const char* name() const noexcept override { return "secs.ii"; }

  std::string message(int ev) const override {
    switch (static_cast<errc>(ev)) {
      case errc::ok:
        return "ok";
      case errc::truncated:
        return "truncated input";
      case errc::invalid_header:
        return "invalid secs-ii header";
      case errc::invalid_format:
        return "invalid secs-ii format code";
      case errc::length_overflow:
        return "secs-ii length overflow";
      case errc::length_mismatch:
        return "secs-ii length mismatch";
      case errc::buffer_overflow:
        return "output buffer overflow";
      default:
        return "unknown secs.ii error";
    }
  }
};

constexpr bool checked_add(std::size_t a, std::size_t b, std::size_t& out) noexcept {
  if (b > (std::numeric_limits<std::size_t>::max() - a)) {
    return false;
  }
  out = a + b;
  return true;
}

constexpr bool checked_mul(std::size_t a, std::size_t b, std::size_t& out) noexcept {
  if (a == 0 || b == 0) {
    out = 0;
    return true;
  }
  if (a > (std::numeric_limits<std::size_t>::max() / b)) {
    return false;
  }
  out = a * b;
  return true;
}

constexpr std::uint8_t length_bytes_for(std::uint32_t length) noexcept {
  if (length <= 0xFFu) {
    return 1;
  }
  if (length <= 0xFFFFu) {
    return 2;
  }
  return 3;
}

constexpr std::uint8_t make_format_byte(format_code code, std::uint8_t length_bytes) noexcept {
  return static_cast<std::uint8_t>((static_cast<std::uint8_t>(code) << 2) | (length_bytes - 1));
}

// 解码深度上限：防止恶意输入构造极深嵌套导致栈溢出。
constexpr std::size_t kMaxDecodeDepth = 64;

class SpanWriter final {
 public:
  explicit SpanWriter(mutable_bytes_view out) : out_(out) {}

  [[nodiscard]] std::size_t written() const noexcept { return written_; }

  std::error_code write_u8(byte v) noexcept {
    if (written_ >= out_.size()) {
      return make_error_code(errc::buffer_overflow);
    }
    out_[written_++] = v;
    return {};
  }

  std::error_code write_bytes(bytes_view v) noexcept {
    if (v.empty()) {
      return {};
    }
    if (out_.size() - written_ < v.size()) {
      return make_error_code(errc::buffer_overflow);
    }
    std::copy(v.begin(), v.end(), out_.begin() + static_cast<std::ptrdiff_t>(written_));
    written_ += v.size();
    return {};
  }

  std::error_code write_be_u32(std::uint32_t v, std::uint8_t bytes) noexcept {
    if (bytes < 1 || bytes > 3) {
      return make_error_code(errc::invalid_header);
    }
    if (out_.size() - written_ < bytes) {
      return make_error_code(errc::buffer_overflow);
    }
    for (std::uint8_t i = 0; i < bytes; ++i) {
      const auto shift = static_cast<std::uint8_t>(8u * (bytes - 1u - i));
      out_[written_ + i] = static_cast<byte>((v >> shift) & 0xFFu);
    }
    written_ += bytes;
    return {};
  }

  template <class UInt>
  std::error_code write_be_uint(UInt v) noexcept {
    for (std::size_t i = 0; i < sizeof(UInt); ++i) {
      const auto shift = static_cast<unsigned>(8u * (sizeof(UInt) - 1u - i));
      auto ec = write_u8(static_cast<byte>((static_cast<std::make_unsigned_t<UInt>>(v) >> shift) & 0xFFu));
      if (ec) {
        return ec;
      }
    }
    return {};
  }

 private:
  mutable_bytes_view out_{};
  std::size_t written_{0};
};

class SpanReader final {
 public:
  explicit SpanReader(bytes_view in) : in_(in) {}

  [[nodiscard]] std::size_t remaining() const noexcept { return in_.size() - pos_; }
  [[nodiscard]] std::size_t consumed() const noexcept { return pos_; }

  std::error_code read_u8(byte& out) noexcept {
    if (pos_ >= in_.size()) {
      return make_error_code(errc::truncated);
    }
    out = in_[pos_++];
    return {};
  }

  std::error_code read_be_u32(std::uint8_t bytes, std::uint32_t& out) noexcept {
    if (bytes < 1 || bytes > 3) {
      return make_error_code(errc::invalid_header);
    }
    if (remaining() < bytes) {
      return make_error_code(errc::truncated);
    }
    std::uint32_t v = 0;
    for (std::uint8_t i = 0; i < bytes; ++i) {
      v = (v << 8) | static_cast<std::uint32_t>(in_[pos_++]);
    }
    out = v;
    return {};
  }

  std::error_code read_payload(std::uint32_t n, bytes_view& out) noexcept {
    if (remaining() < n) {
      return make_error_code(errc::truncated);
    }
    out = in_.subspan(pos_, n);
    pos_ += n;
    return {};
  }

 private:
  bytes_view in_{};
  std::size_t pos_{0};
};

std::error_code encode_item(const Item& item, SpanWriter& w) noexcept;
std::error_code decode_item(SpanReader& r, Item& out, std::size_t depth) noexcept;

std::error_code encode_header_and_length(
  format_code code,
  std::uint32_t length,
  SpanWriter& w) noexcept {
  if (length > kMaxLength) {
    return make_error_code(errc::length_overflow);
  }
  const auto length_bytes = length_bytes_for(length);
  auto ec = w.write_u8(make_format_byte(code, length_bytes));
  if (ec) {
    return ec;
  }
  return w.write_be_u32(length, length_bytes);
}

format_code item_code(const Item& item) noexcept {
  return std::visit(
    [&](const auto& v) -> format_code {
      using T = std::decay_t<decltype(v)>;
      if constexpr (std::is_same_v<T, List>) {
        return format_code::list;
      } else if constexpr (std::is_same_v<T, ASCII>) {
        return format_code::ascii;
      } else if constexpr (std::is_same_v<T, Binary>) {
        return format_code::binary;
      } else if constexpr (std::is_same_v<T, Boolean>) {
        return format_code::boolean;
      } else if constexpr (std::is_same_v<T, I1>) {
        return format_code::i1;
      } else if constexpr (std::is_same_v<T, I2>) {
        return format_code::i2;
      } else if constexpr (std::is_same_v<T, I4>) {
        return format_code::i4;
      } else if constexpr (std::is_same_v<T, I8>) {
        return format_code::i8;
      } else if constexpr (std::is_same_v<T, U1>) {
        return format_code::u1;
      } else if constexpr (std::is_same_v<T, U2>) {
        return format_code::u2;
      } else if constexpr (std::is_same_v<T, U4>) {
        return format_code::u4;
      } else if constexpr (std::is_same_v<T, U8>) {
        return format_code::u8;
      } else if constexpr (std::is_same_v<T, F4>) {
        return format_code::f4;
      } else if constexpr (std::is_same_v<T, F8>) {
        return format_code::f8;
      } else {
        return format_code::binary;
      }
    },
    item.storage());
}

std::error_code numeric_payload_length(std::size_t count, std::size_t elem_size, std::uint32_t& out) noexcept {
  std::size_t bytes = 0;
  if (!checked_mul(count, elem_size, bytes)) {
    return make_error_code(errc::length_overflow);
  }
  if (bytes > kMaxLength) {
    return make_error_code(errc::length_overflow);
  }
  out = static_cast<std::uint32_t>(bytes);
  return {};
}

std::error_code encoded_size_impl(const Item& item, std::size_t& out_size) noexcept {
  std::size_t payload_size = 0;
  std::uint32_t length_value = 0;

  const auto& storage = item.storage();

  if (const auto* v = std::get_if<List>(&storage)) {
    if (v->size() > kMaxLength) {
      return make_error_code(errc::length_overflow);
    }
    length_value = static_cast<std::uint32_t>(v->size());
    for (const auto& child : *v) {
      std::size_t child_size = 0;
      auto ec = encoded_size_impl(child, child_size);
      if (ec) {
        return ec;
      }
      std::size_t next = 0;
      if (!checked_add(payload_size, child_size, next)) {
        return make_error_code(errc::length_overflow);
      }
      payload_size = next;
    }
  } else if (const auto* v_ascii = std::get_if<ASCII>(&storage)) {
    if (v_ascii->value.size() > kMaxLength) {
      return make_error_code(errc::length_overflow);
    }
    payload_size = v_ascii->value.size();
    length_value = static_cast<std::uint32_t>(payload_size);
  } else if (const auto* v_bin = std::get_if<Binary>(&storage)) {
    if (v_bin->value.size() > kMaxLength) {
      return make_error_code(errc::length_overflow);
    }
    payload_size = v_bin->value.size();
    length_value = static_cast<std::uint32_t>(payload_size);
  } else if (const auto* v_bool = std::get_if<Boolean>(&storage)) {
    if (v_bool->values.size() > kMaxLength) {
      return make_error_code(errc::length_overflow);
    }
    payload_size = v_bool->values.size();
    length_value = static_cast<std::uint32_t>(payload_size);
  } else if (const auto* v = std::get_if<I1>(&storage)) {
    auto ec = numeric_payload_length(v->values.size(), sizeof(std::int8_t), length_value);
    if (ec) {
      return ec;
    }
    payload_size = length_value;
  } else if (const auto* v = std::get_if<I2>(&storage)) {
    auto ec = numeric_payload_length(v->values.size(), sizeof(std::int16_t), length_value);
    if (ec) {
      return ec;
    }
    payload_size = length_value;
  } else if (const auto* v = std::get_if<I4>(&storage)) {
    auto ec = numeric_payload_length(v->values.size(), sizeof(std::int32_t), length_value);
    if (ec) {
      return ec;
    }
    payload_size = length_value;
  } else if (const auto* v = std::get_if<I8>(&storage)) {
    auto ec = numeric_payload_length(v->values.size(), sizeof(std::int64_t), length_value);
    if (ec) {
      return ec;
    }
    payload_size = length_value;
  } else if (const auto* v = std::get_if<U1>(&storage)) {
    auto ec = numeric_payload_length(v->values.size(), sizeof(std::uint8_t), length_value);
    if (ec) {
      return ec;
    }
    payload_size = length_value;
  } else if (const auto* v = std::get_if<U2>(&storage)) {
    auto ec = numeric_payload_length(v->values.size(), sizeof(std::uint16_t), length_value);
    if (ec) {
      return ec;
    }
    payload_size = length_value;
  } else if (const auto* v = std::get_if<U4>(&storage)) {
    auto ec = numeric_payload_length(v->values.size(), sizeof(std::uint32_t), length_value);
    if (ec) {
      return ec;
    }
    payload_size = length_value;
  } else if (const auto* v = std::get_if<U8>(&storage)) {
    auto ec = numeric_payload_length(v->values.size(), sizeof(std::uint64_t), length_value);
    if (ec) {
      return ec;
    }
    payload_size = length_value;
  } else if (const auto* v = std::get_if<F4>(&storage)) {
    auto ec = numeric_payload_length(v->values.size(), sizeof(float), length_value);
    if (ec) {
      return ec;
    }
    payload_size = length_value;
  } else if (const auto* v = std::get_if<F8>(&storage)) {
    auto ec = numeric_payload_length(v->values.size(), sizeof(double), length_value);
    if (ec) {
      return ec;
    }
    payload_size = length_value;
  } else {
    return make_error_code(errc::invalid_format);
  }

  const auto header = static_cast<std::size_t>(1 + length_bytes_for(length_value));
  std::size_t total = 0;
  if (!checked_add(header, payload_size, total)) {
    return make_error_code(errc::length_overflow);
  }
  out_size = total;
  return {};
}

std::error_code encode_payload(const Item& item, SpanWriter& w) noexcept {
  return std::visit(
    [&](const auto& v) -> std::error_code {
      using T = std::decay_t<decltype(v)>;
      if constexpr (std::is_same_v<T, List>) {
        for (const auto& child : v) {
          auto ec = encode_item(child, w);
          if (ec) {
            return ec;
          }
        }
        return {};
      } else if constexpr (std::is_same_v<T, ASCII>) {
        return w.write_bytes(bytes_view{reinterpret_cast<const byte*>(v.value.data()), v.value.size()});
      } else if constexpr (std::is_same_v<T, Binary>) {
        return w.write_bytes(bytes_view{v.value.data(), v.value.size()});
      } else if constexpr (std::is_same_v<T, Boolean>) {
        for (bool b : v.values) {
          auto ec = w.write_u8(static_cast<byte>(b ? 0x01 : 0x00));
          if (ec) {
            return ec;
          }
        }
        return {};
      } else if constexpr (std::is_same_v<T, I1>) {
        for (auto x : v.values) {
          auto ec = w.write_u8(static_cast<byte>(static_cast<std::uint8_t>(x)));
          if (ec) {
            return ec;
          }
        }
        return {};
      } else if constexpr (std::is_same_v<T, U1>) {
        for (auto x : v.values) {
          auto ec = w.write_u8(static_cast<byte>(x));
          if (ec) {
            return ec;
          }
        }
        return {};
      } else if constexpr (std::is_same_v<T, I2>) {
        for (auto x : v.values) {
          auto ec = w.write_be_uint<std::uint16_t>(static_cast<std::uint16_t>(x));
          if (ec) {
            return ec;
          }
        }
        return {};
      } else if constexpr (std::is_same_v<T, U2>) {
        for (auto x : v.values) {
          auto ec = w.write_be_uint<std::uint16_t>(x);
          if (ec) {
            return ec;
          }
        }
        return {};
      } else if constexpr (std::is_same_v<T, I4>) {
        for (auto x : v.values) {
          auto ec = w.write_be_uint<std::uint32_t>(static_cast<std::uint32_t>(x));
          if (ec) {
            return ec;
          }
        }
        return {};
      } else if constexpr (std::is_same_v<T, U4>) {
        for (auto x : v.values) {
          auto ec = w.write_be_uint<std::uint32_t>(x);
          if (ec) {
            return ec;
          }
        }
        return {};
      } else if constexpr (std::is_same_v<T, I8>) {
        for (auto x : v.values) {
          auto ec = w.write_be_uint<std::uint64_t>(static_cast<std::uint64_t>(x));
          if (ec) {
            return ec;
          }
        }
        return {};
      } else if constexpr (std::is_same_v<T, U8>) {
        for (auto x : v.values) {
          auto ec = w.write_be_uint<std::uint64_t>(x);
          if (ec) {
            return ec;
          }
        }
        return {};
      } else if constexpr (std::is_same_v<T, F4>) {
        for (auto x : v.values) {
          const auto bits = std::bit_cast<std::uint32_t>(x);
          auto ec = w.write_be_uint<std::uint32_t>(bits);
          if (ec) {
            return ec;
          }
        }
        return {};
      } else if constexpr (std::is_same_v<T, F8>) {
        for (auto x : v.values) {
          const auto bits = std::bit_cast<std::uint64_t>(x);
          auto ec = w.write_be_uint<std::uint64_t>(bits);
          if (ec) {
            return ec;
          }
        }
        return {};
      } else {
        return make_error_code(errc::invalid_format);
      }
    },
    item.storage());
}

std::error_code encode_item(const Item& item, SpanWriter& w) noexcept {
  const auto code = item_code(item);

  std::uint32_t length_value = 0;
  const auto& storage = item.storage();
  if (const auto* v = std::get_if<List>(&storage)) {
    if (v->size() > kMaxLength) {
      return make_error_code(errc::length_overflow);
    }
    length_value = static_cast<std::uint32_t>(v->size());
  } else if (const auto* v = std::get_if<ASCII>(&storage)) {
    if (v->value.size() > kMaxLength) {
      return make_error_code(errc::length_overflow);
    }
    length_value = static_cast<std::uint32_t>(v->value.size());
  } else if (const auto* v = std::get_if<Binary>(&storage)) {
    if (v->value.size() > kMaxLength) {
      return make_error_code(errc::length_overflow);
    }
    length_value = static_cast<std::uint32_t>(v->value.size());
  } else if (const auto* v = std::get_if<Boolean>(&storage)) {
    if (v->values.size() > kMaxLength) {
      return make_error_code(errc::length_overflow);
    }
    length_value = static_cast<std::uint32_t>(v->values.size());
  } else if (const auto* v = std::get_if<I1>(&storage)) {
    auto ec = numeric_payload_length(v->values.size(), sizeof(std::int8_t), length_value);
    if (ec) {
      return ec;
    }
  } else if (const auto* v = std::get_if<I2>(&storage)) {
    auto ec = numeric_payload_length(v->values.size(), sizeof(std::int16_t), length_value);
    if (ec) {
      return ec;
    }
  } else if (const auto* v = std::get_if<I4>(&storage)) {
    auto ec = numeric_payload_length(v->values.size(), sizeof(std::int32_t), length_value);
    if (ec) {
      return ec;
    }
  } else if (const auto* v = std::get_if<I8>(&storage)) {
    auto ec = numeric_payload_length(v->values.size(), sizeof(std::int64_t), length_value);
    if (ec) {
      return ec;
    }
  } else if (const auto* v = std::get_if<U1>(&storage)) {
    auto ec = numeric_payload_length(v->values.size(), sizeof(std::uint8_t), length_value);
    if (ec) {
      return ec;
    }
  } else if (const auto* v = std::get_if<U2>(&storage)) {
    auto ec = numeric_payload_length(v->values.size(), sizeof(std::uint16_t), length_value);
    if (ec) {
      return ec;
    }
  } else if (const auto* v = std::get_if<U4>(&storage)) {
    auto ec = numeric_payload_length(v->values.size(), sizeof(std::uint32_t), length_value);
    if (ec) {
      return ec;
    }
  } else if (const auto* v = std::get_if<U8>(&storage)) {
    auto ec = numeric_payload_length(v->values.size(), sizeof(std::uint64_t), length_value);
    if (ec) {
      return ec;
    }
  } else if (const auto* v = std::get_if<F4>(&storage)) {
    auto ec = numeric_payload_length(v->values.size(), sizeof(float), length_value);
    if (ec) {
      return ec;
    }
  } else if (const auto* v = std::get_if<F8>(&storage)) {
    auto ec = numeric_payload_length(v->values.size(), sizeof(double), length_value);
    if (ec) {
      return ec;
    }
  } else {
    return make_error_code(errc::invalid_format);
  }

  auto ec = encode_header_and_length(code, length_value, w);
  if (ec) {
    return ec;
  }
  return encode_payload(item, w);
}

std::optional<format_code> format_code_from_bits(std::uint8_t bits) noexcept {
  switch (static_cast<format_code>(bits)) {
    case format_code::list:
    case format_code::binary:
    case format_code::boolean:
    case format_code::ascii:
    case format_code::i1:
    case format_code::i2:
    case format_code::i4:
    case format_code::i8:
    case format_code::u1:
    case format_code::u2:
    case format_code::u4:
    case format_code::u8:
    case format_code::f4:
    case format_code::f8:
      return static_cast<format_code>(bits);
    default:
      return std::nullopt;
  }
}

template <class UInt>
UInt read_be_uint(bytes_view payload, std::size_t offset) noexcept {
  UInt v = 0;
  for (std::size_t i = 0; i < sizeof(UInt); ++i) {
    v = static_cast<UInt>((v << 8) | payload[offset + i]);
  }
  return v;
}

template <class Unsigned>
std::error_code decode_unsigned_vector(bytes_view payload, std::vector<Unsigned>& out_vec) noexcept {
  if (payload.size() % sizeof(Unsigned) != 0) {
    return make_error_code(errc::length_mismatch);
  }
  const auto count = payload.size() / sizeof(Unsigned);
  out_vec.clear();
  out_vec.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    out_vec.push_back(read_be_uint<Unsigned>(payload, i * sizeof(Unsigned)));
  }
  return {};
}

template <class Signed, class Unsigned>
std::error_code decode_signed_vector(bytes_view payload, std::vector<Signed>& out_vec) noexcept {
  static_assert(sizeof(Signed) == sizeof(Unsigned));
  if (payload.size() % sizeof(Unsigned) != 0) {
    return make_error_code(errc::length_mismatch);
  }
  const auto count = payload.size() / sizeof(Unsigned);
  out_vec.clear();
  out_vec.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    const auto bits = read_be_uint<Unsigned>(payload, i * sizeof(Unsigned));
    out_vec.push_back(std::bit_cast<Signed>(bits));
  }
  return {};
}

std::error_code decode_item(SpanReader& r, Item& out, std::size_t depth) noexcept {
  if (depth > kMaxDecodeDepth) {
    return make_error_code(errc::invalid_header);
  }

  byte format_byte = 0;
  auto ec = r.read_u8(format_byte);
  if (ec) {
    return ec;
  }

  const auto length_bytes = static_cast<std::uint8_t>((format_byte & 0x03u) + 1u);
  if (length_bytes == 4) {
    return make_error_code(errc::invalid_header);
  }

  std::uint32_t length = 0;
  ec = r.read_be_u32(length_bytes, length);
  if (ec) {
    return ec;
  }
  if (length > kMaxLength) {
    return make_error_code(errc::length_overflow);
  }

  const auto fmt_bits = static_cast<std::uint8_t>(format_byte >> 2);
  const auto fmt = format_code_from_bits(fmt_bits);
  if (!fmt) {
    return make_error_code(errc::invalid_format);
  }

  if (*fmt == format_code::list) {
    List items;
    items.reserve(length);
    for (std::uint32_t i = 0; i < length; ++i) {
      Item child = Item::binary({});  // 占位，后续会被覆盖
      ec = decode_item(r, child, depth + 1);
      if (ec) {
        return ec;
      }
      items.push_back(std::move(child));
    }
    out = Item(std::move(items));
    return {};
  }

  bytes_view payload{};
  ec = r.read_payload(length, payload);
  if (ec) {
    return ec;
  }

  switch (*fmt) {
    case format_code::ascii: {
      std::string s(reinterpret_cast<const char*>(payload.data()), payload.size());
      out = Item(ASCII{std::move(s)});
      return {};
    }
    case format_code::binary: {
      std::vector<byte> v(payload.begin(), payload.end());
      out = Item(Binary{std::move(v)});
      return {};
    }
    case format_code::boolean: {
      std::vector<bool> v;
      v.reserve(payload.size());
      for (byte b : payload) {
        v.push_back(b != 0);
      }
      out = Item(Boolean{std::move(v)});
      return {};
    }
    case format_code::i1: {
      std::vector<std::int8_t> v;
      ec = decode_signed_vector<std::int8_t, std::uint8_t>(payload, v);
      if (ec) {
        return ec;
      }
      out = Item(I1{std::move(v)});
      return {};
    }
    case format_code::u1: {
      std::vector<std::uint8_t> v(payload.begin(), payload.end());
      out = Item(U1{std::move(v)});
      return {};
    }
    case format_code::i2: {
      std::vector<std::int16_t> v;
      ec = decode_signed_vector<std::int16_t, std::uint16_t>(payload, v);
      if (ec) {
        return ec;
      }
      out = Item(I2{std::move(v)});
      return {};
    }
    case format_code::u2: {
      std::vector<std::uint16_t> v;
      ec = decode_unsigned_vector<std::uint16_t>(payload, v);
      if (ec) {
        return ec;
      }
      out = Item(U2{std::move(v)});
      return {};
    }
    case format_code::i4: {
      std::vector<std::int32_t> v;
      ec = decode_signed_vector<std::int32_t, std::uint32_t>(payload, v);
      if (ec) {
        return ec;
      }
      out = Item(I4{std::move(v)});
      return {};
    }
    case format_code::u4: {
      std::vector<std::uint32_t> v;
      ec = decode_unsigned_vector<std::uint32_t>(payload, v);
      if (ec) {
        return ec;
      }
      out = Item(U4{std::move(v)});
      return {};
    }
    case format_code::i8: {
      std::vector<std::int64_t> v;
      ec = decode_signed_vector<std::int64_t, std::uint64_t>(payload, v);
      if (ec) {
        return ec;
      }
      out = Item(I8{std::move(v)});
      return {};
    }
    case format_code::u8: {
      std::vector<std::uint64_t> v;
      ec = decode_unsigned_vector<std::uint64_t>(payload, v);
      if (ec) {
        return ec;
      }
      out = Item(U8{std::move(v)});
      return {};
    }
    case format_code::f4: {
      if (payload.size() % 4 != 0) {
        return make_error_code(errc::length_mismatch);
      }
      const auto count = payload.size() / 4;
      std::vector<float> v;
      v.reserve(count);
      for (std::size_t i = 0; i < count; ++i) {
        const auto bits = read_be_uint<std::uint32_t>(payload, i * 4);
        v.push_back(std::bit_cast<float>(bits));
      }
      out = Item(F4{std::move(v)});
      return {};
    }
    case format_code::f8: {
      if (payload.size() % 8 != 0) {
        return make_error_code(errc::length_mismatch);
      }
      const auto count = payload.size() / 8;
      std::vector<double> v;
      v.reserve(count);
      for (std::size_t i = 0; i < count; ++i) {
        const auto bits = read_be_uint<std::uint64_t>(payload, i * 8);
        v.push_back(std::bit_cast<double>(bits));
      }
      out = Item(F8{std::move(v)});
      return {};
    }
    default:
      return make_error_code(errc::invalid_format);
  }
}

}  // namespace

const std::error_category& error_category() noexcept {
  static secs_ii_error_category category;
  return category;
}

std::error_code make_error_code(errc e) noexcept {
  return {static_cast<int>(e), error_category()};
}

std::error_code encoded_size(const Item& item, std::size_t& out_size) noexcept {
  return encoded_size_impl(item, out_size);
}

std::error_code encode(const Item& item, std::vector<byte>& out) noexcept {
  std::size_t size = 0;
  auto ec = encoded_size(item, size);
  if (ec) {
    return ec;
  }

  const auto offset = out.size();
  if (size > (std::numeric_limits<std::size_t>::max() - offset)) {
    return make_error_code(errc::length_overflow);
  }

  out.reserve(offset + size);
  out.resize(offset + size);
  mutable_bytes_view dest{out.data() + offset, size};

  std::size_t written = 0;
  ec = encode_to(dest, item, written);
  if (ec) {
    out.resize(offset);
    return ec;
  }
  if (written != size) {
    out.resize(offset);
    return make_error_code(errc::invalid_header);
  }
  return {};
}

std::error_code encode_to(mutable_bytes_view out, const Item& item, std::size_t& written) noexcept {
  SpanWriter w(out);
  auto ec = encode_item(item, w);
  written = w.written();
  return ec;
}

std::error_code decode_one(bytes_view in, Item& out, std::size_t& consumed) noexcept {
  SpanReader r(in);
  auto ec = decode_item(r, out, 0);
  if (ec) {
    consumed = 0;
    return ec;
  }
  consumed = r.consumed();
  return {};
}

}  // namespace secs::ii
