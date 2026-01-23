/**
 * @file secs_sml_check.cpp
 * @brief CLI：检查 SML 语法/基础语义，并提供 JSON/AST/stats 输出
 */

#include "secs/sml/ast.hpp"
#include "secs/sml/runtime.hpp"
#include "secs/tools/sml_check.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

using secs::sml::Document;
using secs::sml::SourceSpan;
using secs::sml::TemplateItem;
using secs::sml::VarRef;
using secs::tools::SmlCheckDiagnostic;
using secs::tools::SmlCheckDiagnosticKind;

enum class OutputFormat : std::uint8_t {
    text = 0,
    json = 1,
};

struct Options final {
    OutputFormat format{OutputFormat::text};
    bool verbose{false};
    bool stats{false};
    std::vector<std::string> files{};
};

static void print_usage(const char *argv0) {
    std::cout << "用法：\n"
              << "  " << argv0 << " [options] <files...>\n\n"
              << "选项：\n"
              << "  --verbose            输出 AST（文本）\n"
              << "  --stats              输出 messages/conditions/timers 统计\n"
              << "  --format <text|json> 输出格式（默认 text）\n"
              << "  -h, --help           显示帮助\n";
}

static int parse_args(int argc, char **argv, Options &out) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};

        if (arg == "-h" || arg == "--help") {
            return 1;
        }
        if (arg == "--verbose") {
            out.verbose = true;
            continue;
        }
        if (arg == "--stats") {
            out.stats = true;
            continue;
        }
        if (arg == "--format") {
            if (i + 1 >= argc) {
                std::cerr << "missing value for --format\n";
                return -1;
            }
            const std::string_view v{argv[++i]};
            if (v == "text") {
                out.format = OutputFormat::text;
                continue;
            }
            if (v == "json") {
                out.format = OutputFormat::json;
                continue;
            }
            std::cerr << "invalid --format: " << v << " (expected text|json)\n";
            return -1;
        }

        if (!arg.empty() && arg.front() == '-') {
            std::cerr << "unknown option: " << arg << "\n";
            return -1;
        }

        out.files.emplace_back(arg);
    }

    if (out.files.empty()) {
        std::cerr << "no input files\n";
        return -1;
    }
    return 0;
}

static std::optional<std::string> read_file_text(const std::string &path,
                                                 std::string &out_err) {
    std::ifstream f(path, std::ios::in | std::ios::binary);
    if (!f.is_open()) {
        out_err = "open failed";
        return std::nullopt;
    }
    f.seekg(0, std::ios::end);
    const std::streamoff size = f.tellg();
    if (size < 0) {
        out_err = "tellg failed";
        return std::nullopt;
    }
    f.seekg(0, std::ios::beg);

    std::string buf;
    buf.resize(static_cast<std::size_t>(size));
    if (!buf.empty()) {
        f.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        if (!f) {
            out_err = "read failed";
            return std::nullopt;
        }
    }
    return buf;
}

struct SourceLines final {
    std::string_view src;
    std::vector<std::size_t> starts; // 1-based: starts[line-1]
};

static SourceLines build_source_lines(std::string_view src) {
    SourceLines lines;
    lines.src = src;
    lines.starts.reserve(256);
    lines.starts.push_back(0);
    for (std::size_t i = 0; i < src.size(); ++i) {
        if (src[i] == '\n') {
            lines.starts.push_back(i + 1);
        }
    }
    return lines;
}

static std::string_view line_at(const SourceLines &lines, std::uint32_t line) {
    if (line == 0 || line > lines.starts.size()) {
        return {};
    }
    const std::size_t start = lines.starts[line - 1];
    const std::size_t end =
        (line < lines.starts.size()) ? (lines.starts[line] - 1) : lines.src.size();

    std::size_t real_end = end;
    if (real_end > start && lines.src[real_end - 1] == '\r') {
        --real_end;
    }
    return lines.src.substr(start, real_end - start);
}

static void append_spaces(std::ostream &os, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        os.put(' ');
    }
}

using DiagnosticKind = SmlCheckDiagnosticKind;
using Diagnostic = SmlCheckDiagnostic;

static std::string_view diagnostic_kind_name(DiagnosticKind k) noexcept {
    return secs::tools::sml_check_diagnostic_kind_name(k);
}

struct Stats final {
    std::size_t messages{0};
    std::size_t conditions{0};
    std::size_t timers{0};
};

struct FileReport final {
    std::string path{};
    bool ok{false};
    Stats stats{};
    std::vector<Diagnostic> diags{};
    std::string ast_text{};
};

static void append_indent(std::string &out, int n) {
    out.append(static_cast<std::size_t>(n), ' ');
}

template <class T>
static void dump_value_expr_as_text(const secs::sml::ValueExpr<T> &expr,
                                    std::string &out) {
    if (const auto *lit = std::get_if<T>(&expr)) {
        std::ostringstream oss;
        if constexpr (std::is_same_v<T, secs::ii::byte>) {
            oss << static_cast<unsigned int>(*lit);
        } else {
            oss << *lit;
        }
        out.append(oss.str());
        return;
    }
    if (const auto *ref = std::get_if<VarRef>(&expr)) {
        out.append("${");
        out.append(ref->name);
        out.push_back('}');
        return;
    }
    out.append("<invalid>");
}

static void dump_template_item(const TemplateItem &tpl,
                               std::string &out,
                               int indent) {
    std::visit(
        [&](const auto &alt) {
            using T = std::decay_t<decltype(alt)>;

            if constexpr (std::is_same_v<T, secs::sml::TplList>) {
                append_indent(out, indent);
                out.append("L\n");
                for (const auto &child : alt) {
                    dump_template_item(child, out, indent + 2);
                }
            } else if constexpr (std::is_same_v<T, secs::sml::TplASCII>) {
                append_indent(out, indent);
                out.append("A ");
                if (const auto *s = std::get_if<std::string>(&alt.value)) {
                    out.push_back('"');
                    out.append(*s);
                    out.append("\"\n");
                } else if (const auto *ref = std::get_if<VarRef>(&alt.value)) {
                    out.append("${");
                    out.append(ref->name);
                    out.append("}\n");
                } else {
                    out.append("<invalid>\n");
                }
            } else if constexpr (std::is_same_v<T, secs::sml::TplBinary>) {
                append_indent(out, indent);
                out.append("B [");
                for (std::size_t i = 0; i < alt.values.size(); ++i) {
                    if (i != 0) {
                        out.append(", ");
                    }
                    dump_value_expr_as_text(alt.values[i], out);
                }
                out.append("]\n");
            } else if constexpr (std::is_same_v<T, secs::sml::TplBoolean>) {
                append_indent(out, indent);
                out.append("Boolean [");
                for (std::size_t i = 0; i < alt.values.size(); ++i) {
                    if (i != 0) {
                        out.append(", ");
                    }
                    dump_value_expr_as_text(alt.values[i], out);
                }
                out.append("]\n");
            } else if constexpr (std::is_same_v<T, secs::sml::TplI1>) {
                append_indent(out, indent);
                out.append("I1 [");
                for (std::size_t i = 0; i < alt.values.size(); ++i) {
                    if (i != 0) {
                        out.append(", ");
                    }
                    dump_value_expr_as_text(alt.values[i], out);
                }
                out.append("]\n");
            } else if constexpr (std::is_same_v<T, secs::sml::TplI2>) {
                append_indent(out, indent);
                out.append("I2 [");
                for (std::size_t i = 0; i < alt.values.size(); ++i) {
                    if (i != 0) {
                        out.append(", ");
                    }
                    dump_value_expr_as_text(alt.values[i], out);
                }
                out.append("]\n");
            } else if constexpr (std::is_same_v<T, secs::sml::TplI4>) {
                append_indent(out, indent);
                out.append("I4 [");
                for (std::size_t i = 0; i < alt.values.size(); ++i) {
                    if (i != 0) {
                        out.append(", ");
                    }
                    dump_value_expr_as_text(alt.values[i], out);
                }
                out.append("]\n");
            } else if constexpr (std::is_same_v<T, secs::sml::TplI8>) {
                append_indent(out, indent);
                out.append("I8 [");
                for (std::size_t i = 0; i < alt.values.size(); ++i) {
                    if (i != 0) {
                        out.append(", ");
                    }
                    dump_value_expr_as_text(alt.values[i], out);
                }
                out.append("]\n");
            } else if constexpr (std::is_same_v<T, secs::sml::TplU1>) {
                append_indent(out, indent);
                out.append("U1 [");
                for (std::size_t i = 0; i < alt.values.size(); ++i) {
                    if (i != 0) {
                        out.append(", ");
                    }
                    dump_value_expr_as_text(alt.values[i], out);
                }
                out.append("]\n");
            } else if constexpr (std::is_same_v<T, secs::sml::TplU2>) {
                append_indent(out, indent);
                out.append("U2 [");
                for (std::size_t i = 0; i < alt.values.size(); ++i) {
                    if (i != 0) {
                        out.append(", ");
                    }
                    dump_value_expr_as_text(alt.values[i], out);
                }
                out.append("]\n");
            } else if constexpr (std::is_same_v<T, secs::sml::TplU4>) {
                append_indent(out, indent);
                out.append("U4 [");
                for (std::size_t i = 0; i < alt.values.size(); ++i) {
                    if (i != 0) {
                        out.append(", ");
                    }
                    dump_value_expr_as_text(alt.values[i], out);
                }
                out.append("]\n");
            } else if constexpr (std::is_same_v<T, secs::sml::TplU8>) {
                append_indent(out, indent);
                out.append("U8 [");
                for (std::size_t i = 0; i < alt.values.size(); ++i) {
                    if (i != 0) {
                        out.append(", ");
                    }
                    dump_value_expr_as_text(alt.values[i], out);
                }
                out.append("]\n");
            } else if constexpr (std::is_same_v<T, secs::sml::TplF4>) {
                append_indent(out, indent);
                out.append("F4 [");
                for (std::size_t i = 0; i < alt.values.size(); ++i) {
                    if (i != 0) {
                        out.append(", ");
                    }
                    dump_value_expr_as_text(alt.values[i], out);
                }
                out.append("]\n");
            } else if constexpr (std::is_same_v<T, secs::sml::TplF8>) {
                append_indent(out, indent);
                out.append("F8 [");
                for (std::size_t i = 0; i < alt.values.size(); ++i) {
                    if (i != 0) {
                        out.append(", ");
                    }
                    dump_value_expr_as_text(alt.values[i], out);
                }
                out.append("]\n");
            }
        },
        tpl.storage());
}

static std::string dump_document_text(const Document &doc) {
    std::string out;
    out.reserve(4096);

    out.append("AST:\n");
    out.append("  messages: ");
    out.append(std::to_string(doc.messages.size()));
    out.append("\n");
    for (const auto &msg : doc.messages) {
        out.append("  - ");
        if (!msg.name.empty()) {
            out.append("name=");
            out.append(msg.name);
            out.append(" ");
        } else {
            out.append("name=<anon> ");
        }
        out.append("sf=S");
        out.append(std::to_string(msg.stream));
        out.append("F");
        out.append(std::to_string(msg.function));
        out.append(" w=");
        out.append(msg.w_bit ? "1" : "0");
        out.append("\n");
        dump_template_item(msg.item, out, 4);
    }

    out.append("  conditions: ");
    out.append(std::to_string(doc.conditions.size()));
    out.append("\n");
    for (const auto &rule : doc.conditions) {
        out.append("  - if (");
        out.append(rule.condition.message_name);
        out.append(") ");
        out.append(rule.response_name);
        out.append("\n");
    }

    out.append("  timers: ");
    out.append(std::to_string(doc.timers.size()));
    out.append("\n");
    for (const auto &t : doc.timers) {
        out.append("  - every ");
        out.append(std::to_string(t.interval_seconds));
        out.append(" send ");
        out.append(t.message_name);
        out.append("\n");
    }

    return out;
}

static void print_diagnostic_text(const SourceLines &lines,
                                  const Diagnostic &d) {
    std::cout << "  Line " << d.span.line << ", Column " << d.span.column << ": ";
    switch (d.kind) {
    case DiagnosticKind::io_error:
        std::cout << "IO error\n";
        break;
    case DiagnosticKind::syntax_error:
        std::cout << "Syntax error\n";
        break;
    case DiagnosticKind::undefined_message_reference:
        std::cout << "Undefined message reference\n";
        break;
    case DiagnosticKind::type_mismatch:
        std::cout << "Type mismatch\n";
        break;
    }

    const auto line = line_at(lines, d.span.line);
    if (!line.empty() && d.span.column > 0) {
        std::cout << "    " << line << "\n";
        std::cout << "    ";

        const std::size_t col0 =
            (d.span.column > 0) ? static_cast<std::size_t>(d.span.column - 1) : 0;
        const std::size_t caret_pos =
            std::min(col0, static_cast<std::size_t>(line.size()));
        append_spaces(std::cout, caret_pos);

        const std::size_t caret_len =
            std::max<std::size_t>(1u, static_cast<std::size_t>(d.span.length));
        for (std::size_t i = 0; i < caret_len; ++i) {
            std::cout.put('^');
        }
        std::cout << "\n";
    }

    std::cout << "    " << d.message << "\n";
    if (!d.note.empty()) {
        std::cout << "    " << d.note << "\n";
    }
}

static void json_escape_to(std::ostream &os, std::string_view s) {
    for (const char c : s) {
        switch (c) {
        case '\\':
            os << "\\\\";
            break;
        case '"':
            os << "\\\"";
            break;
        case '\n':
            os << "\\n";
            break;
        case '\r':
            os << "\\r";
            break;
        case '\t':
            os << "\\t";
            break;
        default:
            os << c;
            break;
        }
    }
}

static void json_string(std::ostream &os, std::string_view s) {
    os.put('"');
    json_escape_to(os, s);
    os.put('"');
}

static void write_report_json(std::ostream &os,
                              const std::vector<FileReport> &reports) {
    bool all_ok = true;
    for (const auto &r : reports) {
        all_ok = all_ok && r.ok;
    }

    os << "{";
    os << "\"ok\":" << (all_ok ? "true" : "false") << ",";
    os << "\"files\":[";

    for (std::size_t i = 0; i < reports.size(); ++i) {
        if (i != 0) {
            os << ",";
        }
        const auto &r = reports[i];
        os << "{";
        os << "\"path\":";
        json_string(os, r.path);
        os << ",\"ok\":" << (r.ok ? "true" : "false");

        os << ",\"stats\":{\"messages\":" << r.stats.messages
           << ",\"conditions\":" << r.stats.conditions
           << ",\"timers\":" << r.stats.timers << "}";

        os << ",\"diagnostics\":[";
        for (std::size_t j = 0; j < r.diags.size(); ++j) {
            if (j != 0) {
                os << ",";
            }
            const auto &d = r.diags[j];
            os << "{";
            os << "\"kind\":";
            json_string(os, diagnostic_kind_name(d.kind));
            os << ",\"line\":" << d.span.line;
            os << ",\"column\":" << d.span.column;
            os << ",\"length\":" << d.span.length;
            os << ",\"message\":";
            json_string(os, d.message);
            if (!d.note.empty()) {
                os << ",\"note\":";
                json_string(os, d.note);
            }
            if (!d.category.empty()) {
                os << ",\"category\":";
                json_string(os, d.category);
            }
            if (d.code != 0) {
                os << ",\"code\":" << d.code;
            }
            os << "}";
        }
        os << "]";

        if (!r.ast_text.empty()) {
            os << ",\"ast_text\":";
            json_string(os, r.ast_text);
        }

        os << "}";
    }

    os << "]";
    os << "}";
}

static FileReport check_one_file(const std::string &path,
                                 const Options &opt) {
    FileReport rep;
    rep.path = path;

    std::string io_err;
    const auto content_opt = read_file_text(path, io_err);
    if (!content_opt.has_value()) {
        Diagnostic d;
        d.kind = DiagnosticKind::io_error;
        d.message = "Cannot read file: " + io_err;
        rep.diags.push_back(std::move(d));
        rep.ok = false;
        return rep;
    }

    const auto &content = *content_opt;
    const auto parsed = secs::sml::parse_sml(content);
    if (parsed.ec) {
        Diagnostic d;
        d.kind = DiagnosticKind::syntax_error;
        d.span = SourceSpan{parsed.error_line, parsed.error_column, 1};
        d.message = parsed.error_message;
        d.category = parsed.ec.category().name();
        d.code = parsed.ec.value();
        rep.diags.push_back(std::move(d));
        rep.ok = false;
        return rep;
    }

    const Document &doc = parsed.document;
    rep.stats.messages = doc.messages.size();
    rep.stats.conditions = doc.conditions.size();
    rep.stats.timers = doc.timers.size();

    secs::tools::check_undefined_message_refs(doc, rep.diags);
    secs::tools::check_variable_type_consistency(doc, rep.diags);

    if (opt.verbose) {
        rep.ast_text = dump_document_text(doc);
    }

    rep.ok = rep.diags.empty();
    return rep;
}

} // namespace

int main(int argc, char **argv) {
    Options opt{};
    const int parse_rc = parse_args(argc, argv, opt);
    if (parse_rc != 0) {
        if (parse_rc > 0) {
            print_usage(argv[0]);
            return 0;
        }
        print_usage(argv[0]);
        return 2;
    }

    std::vector<FileReport> reports;
    reports.reserve(opt.files.size());

    bool all_ok = true;
    for (const auto &path : opt.files) {
        auto r = check_one_file(path, opt);
        all_ok = all_ok && r.ok;
        reports.push_back(std::move(r));
    }

    if (opt.format == OutputFormat::json) {
        write_report_json(std::cout, reports);
        std::cout << "\n";
        return all_ok ? 0 : 1;
    }

    for (const auto &r : reports) {
        std::cout << (r.ok ? "✓ " : "✗ ") << r.path << "\n";
        if (r.ok && opt.stats) {
            std::cout << "  - " << r.stats.messages << " messages defined\n";
            std::cout << "  - " << r.stats.conditions << " conditions defined\n";
            std::cout << "  - " << r.stats.timers << " timers defined\n";
            std::cout << "  - No syntax errors\n";
        }

        if (!r.diags.empty()) {
            std::string io_err;
            const auto content_opt = read_file_text(r.path, io_err);
            const auto lines =
                build_source_lines(content_opt.value_or(std::string{}));
            for (const auto &d : r.diags) {
                print_diagnostic_text(lines, d);
                std::cout << "\n";
            }
        }

        if (!r.ast_text.empty()) {
            std::cout << r.ast_text << "\n";
        }
    }

    return all_ok ? 0 : 1;
}
