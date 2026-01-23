#include "secs/tools/sml_check.hpp"

#include <charconv>
#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace secs::tools {
namespace {

using secs::sml::CaptureVar;
using secs::sml::Document;
using secs::sml::PatternItem;
using secs::sml::SourceSpan;
using secs::sml::TemplateItem;
using secs::sml::ValueExpr;
using secs::sml::VarRef;

[[nodiscard]] bool parse_sf(std::string_view name,
                            std::uint8_t &stream,
                            std::uint8_t &function) noexcept {
    if (name.size() < 4) {
        return false;
    }
    if (name[0] != 'S' && name[0] != 's') {
        return false;
    }

    const auto f_pos = name.find_first_of("Ff");
    if (f_pos == std::string_view::npos || f_pos < 2) {
        return false;
    }

    int s = 0;
    int f = 0;
    const auto *s_begin = name.data() + 1;
    const auto *s_end = name.data() + f_pos;
    auto [s_ptr, s_ec] = std::from_chars(s_begin, s_end, s);
    if (s_ec != std::errc{} || s_ptr != s_end) {
        return false;
    }

    const auto *f_begin = name.data() + f_pos + 1;
    const auto *f_end = name.data() + name.size();
    auto [f_ptr, f_ec] = std::from_chars(f_begin, f_end, f);
    if (f_ec != std::errc{} || f_ptr != f_end) {
        return false;
    }

    if (s < 0 || s > 127 || f < 0 || f > 255) {
        return false;
    }
    stream = static_cast<std::uint8_t>(s);
    function = static_cast<std::uint8_t>(f);
    return true;
}

struct MessageIndex final {
    std::unordered_set<std::string> names;
    std::unordered_set<std::uint16_t> sfs;
};

MessageIndex build_message_index(const Document &doc) {
    MessageIndex idx;
    idx.names.reserve(doc.messages.size());
    idx.sfs.reserve(doc.messages.size());
    for (const auto &msg : doc.messages) {
        if (!msg.name.empty()) {
            idx.names.insert(msg.name);
        }
        const std::uint16_t key =
            (static_cast<std::uint16_t>(msg.stream) << 8) |
            static_cast<std::uint16_t>(msg.function);
        idx.sfs.insert(key);
    }
    return idx;
}

bool resolves_to_message(const MessageIndex &idx, std::string_view name_or_sf) {
    std::uint8_t stream = 0;
    std::uint8_t function = 0;
    if (parse_sf(name_or_sf, stream, function)) {
        const std::uint16_t key =
            (static_cast<std::uint16_t>(stream) << 8) |
            static_cast<std::uint16_t>(function);
        return idx.sfs.contains(key);
    }
    return idx.names.contains(std::string{name_or_sf});
}

enum class VarType : std::uint8_t {
    unknown,
    list,
    ascii,
    binary,
    boolean,
    i1,
    i2,
    i4,
    i8,
    u1,
    u2,
    u4,
    u8,
    f4,
    f8,
};

std::string_view var_type_name(VarType t) noexcept {
    switch (t) {
    case VarType::unknown:
        return "Unknown";
    case VarType::list:
        return "L";
    case VarType::ascii:
        return "A";
    case VarType::binary:
        return "B";
    case VarType::boolean:
        return "Boolean";
    case VarType::i1:
        return "I1";
    case VarType::i2:
        return "I2";
    case VarType::i4:
        return "I4";
    case VarType::i8:
        return "I8";
    case VarType::u1:
        return "U1";
    case VarType::u2:
        return "U2";
    case VarType::u4:
        return "U4";
    case VarType::u8:
        return "U8";
    case VarType::f4:
        return "F4";
    case VarType::f8:
        return "F8";
    }
    return "Unknown";
}

struct VarConstraint final {
    VarType type{VarType::unknown};
    SourceSpan first_span{};
};

bool is_span_valid(const SourceSpan &s) noexcept {
    return s.line != 0 && s.column != 0;
}

void unify_var_constraint(std::unordered_map<std::string, VarConstraint> &vars,
                          std::string_view name,
                          VarType required,
                          const SourceSpan &span,
                          std::vector<SmlCheckDiagnostic> &out_diags) {
    auto it = vars.find(std::string{name});
    if (it == vars.end()) {
        VarConstraint c;
        c.type = required;
        c.first_span = span;
        vars.emplace(std::string{name}, c);
        return;
    }

    VarConstraint &c = it->second;
    if (c.type == VarType::unknown) {
        c.type = required;
        c.first_span = span;
        return;
    }

    if (c.type == required) {
        return;
    }

    SmlCheckDiagnostic d;
    d.kind = SmlCheckDiagnosticKind::type_mismatch;
    d.span = span;

    std::ostringstream oss;
    oss << "Type mismatch for variable '" << name << "': expected "
        << var_type_name(c.type) << " but used as " << var_type_name(required);
    if (is_span_valid(c.first_span)) {
        oss << " (first seen at line " << c.first_span.line << ", column "
            << c.first_span.column << ")";
    }
    d.message = oss.str();
    out_diags.push_back(std::move(d));
}

template <class T>
void collect_var_refs_from_expr(
    const ValueExpr<T> &expr,
    VarType required,
    std::unordered_map<std::string, VarConstraint> &vars,
    std::vector<SmlCheckDiagnostic> &out_diags) {
    if (const auto *ref = std::get_if<VarRef>(&expr)) {
        unify_var_constraint(vars, ref->name, required, ref->span, out_diags);
    }
}

void collect_var_refs_from_template(
    const TemplateItem &tpl,
    std::unordered_map<std::string, VarConstraint> &vars,
    std::vector<SmlCheckDiagnostic> &out_diags) {
    std::visit(
        [&](const auto &alt) {
            using T = std::decay_t<decltype(alt)>;

            if constexpr (std::is_same_v<T, secs::sml::TplList>) {
                for (const auto &child : alt) {
                    collect_var_refs_from_template(child, vars, out_diags);
                }
            } else if constexpr (std::is_same_v<T, secs::sml::TplASCII>) {
                if (const auto *ref = std::get_if<VarRef>(&alt.value)) {
                    unify_var_constraint(vars,
                                         ref->name,
                                         VarType::ascii,
                                         ref->span,
                                         out_diags);
                }
            } else if constexpr (std::is_same_v<T, secs::sml::TplBinary>) {
                for (const auto &expr : alt.values) {
                    collect_var_refs_from_expr(expr,
                                               VarType::binary,
                                               vars,
                                               out_diags);
                }
            } else if constexpr (std::is_same_v<T, secs::sml::TplBoolean>) {
                for (const auto &expr : alt.values) {
                    collect_var_refs_from_expr(expr,
                                               VarType::boolean,
                                               vars,
                                               out_diags);
                }
            } else if constexpr (std::is_same_v<T, secs::sml::TplI1>) {
                for (const auto &expr : alt.values) {
                    collect_var_refs_from_expr(expr, VarType::i1, vars, out_diags);
                }
            } else if constexpr (std::is_same_v<T, secs::sml::TplI2>) {
                for (const auto &expr : alt.values) {
                    collect_var_refs_from_expr(expr, VarType::i2, vars, out_diags);
                }
            } else if constexpr (std::is_same_v<T, secs::sml::TplI4>) {
                for (const auto &expr : alt.values) {
                    collect_var_refs_from_expr(expr, VarType::i4, vars, out_diags);
                }
            } else if constexpr (std::is_same_v<T, secs::sml::TplI8>) {
                for (const auto &expr : alt.values) {
                    collect_var_refs_from_expr(expr, VarType::i8, vars, out_diags);
                }
            } else if constexpr (std::is_same_v<T, secs::sml::TplU1>) {
                for (const auto &expr : alt.values) {
                    collect_var_refs_from_expr(expr, VarType::u1, vars, out_diags);
                }
            } else if constexpr (std::is_same_v<T, secs::sml::TplU2>) {
                for (const auto &expr : alt.values) {
                    collect_var_refs_from_expr(expr, VarType::u2, vars, out_diags);
                }
            } else if constexpr (std::is_same_v<T, secs::sml::TplU4>) {
                for (const auto &expr : alt.values) {
                    collect_var_refs_from_expr(expr, VarType::u4, vars, out_diags);
                }
            } else if constexpr (std::is_same_v<T, secs::sml::TplU8>) {
                for (const auto &expr : alt.values) {
                    collect_var_refs_from_expr(expr, VarType::u8, vars, out_diags);
                }
            } else if constexpr (std::is_same_v<T, secs::sml::TplF4>) {
                for (const auto &expr : alt.values) {
                    collect_var_refs_from_expr(expr, VarType::f4, vars, out_diags);
                }
            } else if constexpr (std::is_same_v<T, secs::sml::TplF8>) {
                for (const auto &expr : alt.values) {
                    collect_var_refs_from_expr(expr, VarType::f8, vars, out_diags);
                }
            }
        },
        tpl.storage());
}

void collect_capture_vars_from_pattern(
    const PatternItem &pat,
    std::unordered_map<std::string, VarConstraint> &vars,
    std::vector<SmlCheckDiagnostic> &out_diags) {
    std::visit(
        [&](const auto &alt) {
            using T = std::decay_t<decltype(alt)>;

            const auto add_capture =
                [&](const std::optional<CaptureVar> &cap, VarType required) {
                    if (!cap.has_value()) {
                        return;
                    }
                    unify_var_constraint(vars,
                                         cap->name,
                                         required,
                                         cap->span,
                                         out_diags);
                };

            if constexpr (std::is_same_v<T, secs::sml::PatL>) {
                add_capture(alt.capture, VarType::list);
                for (const auto &child : alt.items) {
                    collect_capture_vars_from_pattern(child, vars, out_diags);
                }
            } else if constexpr (std::is_same_v<T, secs::sml::PatASCII>) {
                add_capture(alt.capture, VarType::ascii);
            } else if constexpr (std::is_same_v<T, secs::sml::PatBinary>) {
                add_capture(alt.capture, VarType::binary);
            } else if constexpr (std::is_same_v<T, secs::sml::PatBoolean>) {
                add_capture(alt.capture, VarType::boolean);
            } else if constexpr (std::is_same_v<T, secs::sml::PatI1>) {
                add_capture(alt.capture, VarType::i1);
            } else if constexpr (std::is_same_v<T, secs::sml::PatI2>) {
                add_capture(alt.capture, VarType::i2);
            } else if constexpr (std::is_same_v<T, secs::sml::PatI4>) {
                add_capture(alt.capture, VarType::i4);
            } else if constexpr (std::is_same_v<T, secs::sml::PatI8>) {
                add_capture(alt.capture, VarType::i8);
            } else if constexpr (std::is_same_v<T, secs::sml::PatU1>) {
                add_capture(alt.capture, VarType::u1);
            } else if constexpr (std::is_same_v<T, secs::sml::PatU2>) {
                add_capture(alt.capture, VarType::u2);
            } else if constexpr (std::is_same_v<T, secs::sml::PatU4>) {
                add_capture(alt.capture, VarType::u4);
            } else if constexpr (std::is_same_v<T, secs::sml::PatU8>) {
                add_capture(alt.capture, VarType::u8);
            } else if constexpr (std::is_same_v<T, secs::sml::PatF4>) {
                add_capture(alt.capture, VarType::f4);
            } else if constexpr (std::is_same_v<T, secs::sml::PatF8>) {
                add_capture(alt.capture, VarType::f8);
            }
        },
        pat.storage());
}

} // namespace

std::string_view
sml_check_diagnostic_kind_name(SmlCheckDiagnosticKind k) noexcept {
    switch (k) {
    case SmlCheckDiagnosticKind::io_error:
        return "io_error";
    case SmlCheckDiagnosticKind::syntax_error:
        return "syntax_error";
    case SmlCheckDiagnosticKind::undefined_message_reference:
        return "undefined_message_reference";
    case SmlCheckDiagnosticKind::type_mismatch:
        return "type_mismatch";
    }
    return "unknown";
}

void check_undefined_message_refs(const secs::sml::Document &doc,
                                  std::vector<SmlCheckDiagnostic> &out_diags) {
    const auto idx = build_message_index(doc);

    for (const auto &rule : doc.conditions) {
        // 条件侧：
        // - 若写的是 SxFy，Runtime 会直接匹配 stream/function，不要求该 SF 有模板定义；
        // - 若写的是 message name，则必须能从 messages 中解析出其 S/F，否则规则永不命中。
        {
            std::uint8_t s = 0;
            std::uint8_t f = 0;
            if (!parse_sf(rule.condition.message_name, s, f)) {
                if (!idx.names.contains(rule.condition.message_name)) {
                    SmlCheckDiagnostic d;
                    d.kind = SmlCheckDiagnosticKind::undefined_message_reference;
                    d.span = rule.condition.message_span;
                    d.message =
                        "Condition message '" + rule.condition.message_name +
                        "' is not defined";
                    out_diags.push_back(std::move(d));
                }
            }
        }

        // 响应侧：必须能 resolve 到模板，否则后续 encode_message_body 会失败。
        if (!resolves_to_message(idx, rule.response_name)) {
            SmlCheckDiagnostic d;
            d.kind = SmlCheckDiagnosticKind::undefined_message_reference;
            d.span = rule.response_span;
            d.message = "Message '" + rule.response_name + "' is not defined";
            out_diags.push_back(std::move(d));
        }
    }

    for (const auto &rule : doc.timers) {
        if (!resolves_to_message(idx, rule.message_name)) {
            SmlCheckDiagnostic d;
            d.kind = SmlCheckDiagnosticKind::undefined_message_reference;
            d.span = rule.message_span;
            d.message = "Message '" + rule.message_name + "' is not defined";
            out_diags.push_back(std::move(d));
        }
    }
}

void check_variable_type_consistency(
    const secs::sml::Document &doc,
    std::vector<SmlCheckDiagnostic> &out_diags) {
    std::unordered_map<std::string, VarConstraint> vars;
    vars.reserve(64);

    for (const auto &msg : doc.messages) {
        collect_var_refs_from_template(msg.item, vars, out_diags);
    }

    for (const auto &rule : doc.conditions) {
        if (rule.condition.expected.has_value()) {
            collect_var_refs_from_template(
                *rule.condition.expected, vars, out_diags);
        }
        if (rule.condition.pattern.has_value()) {
            collect_capture_vars_from_pattern(*rule.condition.pattern,
                                              vars,
                                              out_diags);
        }
    }
}

} // namespace secs::tools

