#pragma once

#include <system_error>

namespace secs::core {

enum class errc : int {
  ok = 0,
  timeout = 1,
  cancelled = 2,
  buffer_overflow = 3,
  invalid_argument = 4,
};

const std::error_category& error_category() noexcept;
std::error_code make_error_code(errc e) noexcept;

}  // namespace secs::core

namespace std {
template <>
struct is_error_code_enum<secs::core::errc> : true_type {};
}  // namespace std
