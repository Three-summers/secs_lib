#pragma once

#include "secs/ii/item.hpp"
#include "secs/sml/ast.hpp"

#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>

namespace secs::sml {

/**
 * @brief SMLX 模板渲染错误码
 */
enum class render_errc : int {
    ok = 0,
    missing_variable = 1, // 变量未提供
    type_mismatch = 2,    // 变量类型与占位符位置不匹配
};

const std::error_category &render_error_category() noexcept;
std::error_code make_error_code(render_errc e) noexcept;

/**
 * @brief 渲染上下文：变量名 -> SECS-II Item
 *
 * 约定：
 * - 变量值使用 `secs::ii::Item` 表达，以保持“类型对齐 SECS-II”；
 * - 例如：`MDLN` 应提供为 `<A "...">`；`SVIDS` 应提供为 `<U2 ...>`。
 */
class RenderContext final {
public:
    RenderContext() = default;

    void clear() noexcept { vars_.clear(); }

    void set(std::string name, secs::ii::Item value) {
        vars_.insert_or_assign(std::move(name), std::move(value));
    }

    [[nodiscard]] const secs::ii::Item *get(std::string_view name) const noexcept {
        auto it = vars_.find(name);
        if (it == vars_.end()) {
            return nullptr;
        }
        return &it->second;
    }

private:
    struct TransparentStringHash {
        using is_transparent = void;

        std::size_t operator()(std::string_view sv) const noexcept {
            return std::hash<std::string_view>{}(sv);
        }
        std::size_t operator()(const std::string &s) const noexcept {
            return std::hash<std::string_view>{}(s);
        }
    };

    std::unordered_map<std::string,
                       secs::ii::Item,
                       TransparentStringHash,
                       std::equal_to<>>
        vars_{};
};

/**
 * @brief 将 SMLX TemplateItem 渲染为 SECS-II Item
 *
 * 行为：
 * - 字面量按原样输出；
 * - 值位置的 VarRef 会在 ctx 中查找同名变量，并按目标类型展开拼接；
 * - 若变量缺失或类型不匹配，返回 render_errc。
 */
[[nodiscard]] std::error_code
render_item(const TemplateItem &tpl,
            const RenderContext &ctx,
            secs::ii::Item &out) noexcept;

} // namespace secs::sml

namespace std {
template <>
struct is_error_code_enum<secs::sml::render_errc> : true_type {};
} // namespace std

