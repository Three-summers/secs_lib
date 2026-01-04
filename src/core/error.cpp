#include "secs/core/error.hpp"

#include <string>

namespace secs::core {
namespace {

/*
 * core::errc 的 std::error_category 实现。
 *
 * 目的：
 * - 提供库内“通用错误码”的统一错误域（secs.core），用于跨模块返回 std::error_code；
 * - 与协议层错误（例如 secs.secs1 / secs.ii）相互独立，便于调用方按域分类处理。
 *
 * 注意：
 * - message() 返回英文描述，仅用于调试/日志；
 * - 不参与任何协议 on-wire 交互，不影响互操作语义。
 */
class secs_error_category final : public std::error_category {
public:
    const char *name() const noexcept override { return "secs.core"; }

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

} // namespace

const std::error_category &error_category() noexcept {
    static secs_error_category category;
    return category;
}

std::error_code make_error_code(errc e) noexcept {
    return {static_cast<int>(e), error_category()};
}

} // namespace secs::core
