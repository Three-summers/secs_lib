#pragma once

#include <system_error>

namespace secs::core {

/**
 * @brief 本库通用错误码（跨模块复用）。
 *
 * 约定：
 * - 所有异步/协程接口优先返回 std::error_code，避免异常路径。
 * - timeout/cancelled 用于描述“等待类 API”的典型结果：
 *   - timeout：超过指定等待时间
 *   - cancelled：被 stop()/cancel() 主动取消
 */
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
