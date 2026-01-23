#pragma once

#include "secs/sml/ast.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace secs::tools {

/**
 * @brief SML 检查诊断类型（供 CLI/测试复用）。
 */
enum class SmlCheckDiagnosticKind : std::uint8_t {
    io_error,
    syntax_error,
    undefined_message_reference,
    type_mismatch,
};

/**
 * @brief 诊断类型名（用于 JSON 输出或日志）。
 */
[[nodiscard]] std::string_view
sml_check_diagnostic_kind_name(SmlCheckDiagnosticKind k) noexcept;

/**
 * @brief 单条诊断信息（面向 CLI 输出与测试断言）。
 */
struct SmlCheckDiagnostic final {
    SmlCheckDiagnosticKind kind{SmlCheckDiagnosticKind::syntax_error};
    secs::sml::SourceSpan span{};
    std::string message{};
    std::string note{};
    std::string category{};
    int code{0};
};

/**
 * @brief 检查：条件/timer 引用的消息是否可解析到 message template。
 *
 * 行为约定与 `secs-sml-check` 一致：
 * - 条件侧 message_name 若写 SxFy：不要求该 SF 有模板定义；
 * - 条件侧 message_name 若写 message name：必须在 messages 中定义；
 * - 响应侧与 timer：必须能 resolve 到模板，否则后续 encode 会失败。
 */
void check_undefined_message_refs(const secs::sml::Document &doc,
                                  std::vector<SmlCheckDiagnostic> &out_diags);

/**
 * @brief 检查：占位符变量在模板/条件中使用的 Item 类型是否一致。
 *
 * 说明：
 * - 模板：`${NAME}` 作为某种 Item 类型的值（如 U4/A/Binary）；
 * - 条件 pattern：`$NAME` 捕获的 Item 类型（如 `<U4 $CEID>`）；
 * - 若同名变量出现类型不一致，会输出 type_mismatch 诊断。
 */
void check_variable_type_consistency(
    const secs::sml::Document &doc,
    std::vector<SmlCheckDiagnostic> &out_diags);

} // namespace secs::tools

