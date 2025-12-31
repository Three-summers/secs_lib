#include "secs/core/error.hpp"

#include <string>

namespace secs::core {
namespace {

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

}  // namespace

const std::error_category& error_category() noexcept {
  static secs_error_category category;
  return category;
}

std::error_code make_error_code(errc e) noexcept {
  return {static_cast<int>(e), error_category()};
}

}  // namespace secs::core
