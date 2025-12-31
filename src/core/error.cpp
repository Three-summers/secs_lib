#include "secs/core/error.hpp"

#include <string>

namespace secs::core {
namespace {

// core::errc 的 std::error_category 实现：
// - name() 用于区分错误域
// - message() 返回可读的英文描述（便于调试与日志；不参与协议交互）
class secs_error_category final : public std::error_category {
 public:
  const char* name() const noexcept override { return "secs.core"; }

  std::string message(int ev) const override {
    switch (static_cast<errc>(ev)) {
      case errc::ok:
        return "ok";
      case errc::timeout:
        return "timeout";
      case errc::cancelled:
        return "cancelled";
      case errc::buffer_overflow:
        return "buffer overflow";
      case errc::invalid_argument:
        return "invalid argument";
      default:
        return "unknown secs.core error";
    }
  }
};

}  // 匿名命名空间

const std::error_category& error_category() noexcept {
  static secs_error_category category;
  return category;
}

std::error_code make_error_code(errc e) noexcept {
  return {static_cast<int>(e), error_category()};
}

}  // 命名空间 secs::core
