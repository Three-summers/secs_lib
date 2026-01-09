#include "secs/sml/parser.hpp"

#include <charconv>
#include <cstdlib>
#include <limits>

namespace secs::sml {

/*
 * SML（SECS Message Language）语法分析器实现。
 *
 * 输入/输出：
 * - 输入：Lexer 产生的 Token 序列
 * - 输出：Document AST（消息模板、定时规则、条件响应规则等）
 *
 * 解析要点：
 * - 支持解析 SxFy（例如 S1F1 / S15F32）；
 * - 支持数值字面量（十进制与 0x 十六进制）以及浮点数；
 * - 解析失败时返回 parser_errc，并携带 line/column + 错误信息，便于用户定位。
 *
 * 说明：
 * - 该解析器以“可读性优先”为目标，错误恢复策略较保守（遇错尽早返回），
 *   适合在配置加载阶段快速暴露问题。
 */

namespace {

class ParserErrorCategory : public std::error_category {
public:
    [[nodiscard]] const char *name() const noexcept override {
        return "sml.parser";
    }

    [[nodiscard]] std::string message(int ev) const override {
        switch (static_cast<parser_errc>(ev)) {
        case parser_errc::ok:
            return "success";
        case parser_errc::unexpected_token:
            return "unexpected token";
        case parser_errc::expected_item:
            return "expected item";
        case parser_errc::expected_identifier:
            return "expected identifier";
        case parser_errc::expected_number:
            return "expected number";
        case parser_errc::invalid_stream_function:
            return "invalid stream/function format";
        case parser_errc::unclosed_item:
            return "unclosed item";
        case parser_errc::invalid_condition:
            return "invalid condition";
        }
        return "unknown parser error";
    }
};

const ParserErrorCategory kParserErrorCategory{};

// 解析 SxFy 格式（例如 S1F1、S15F32）
bool parse_sf_string(std::string_view text,
                     std::uint8_t &stream,
                     std::uint8_t &function) {
    if (text.size() < 4)
        return false;

    // 支持 'S1F1' 或 S1F1 格式
    std::string_view sv = text;
    if (sv.front() == '\'' && sv.back() == '\'') {
        sv = sv.substr(1, sv.size() - 2);
    }

    if (sv.empty() || (sv[0] != 'S' && sv[0] != 's'))
        return false;

    std::size_t f_pos = sv.find_first_of("Ff");
    if (f_pos == std::string_view::npos || f_pos < 2)
        return false;

    std::string_view stream_str = sv.substr(1, f_pos - 1);
    std::string_view func_str = sv.substr(f_pos + 1);

    int s = 0, f = 0;
    auto [ptr1, ec1] = std::from_chars(
        stream_str.data(), stream_str.data() + stream_str.size(), s);
    auto [ptr2, ec2] =
        std::from_chars(func_str.data(), func_str.data() + func_str.size(), f);

    if (ec1 != std::errc{} || ec2 != std::errc{})
        return false;
    if (s < 0 || s > 127 || f < 0 || f > 255)
        return false;

    stream = static_cast<std::uint8_t>(s);
    function = static_cast<std::uint8_t>(f);
    return true;
}

[[nodiscard]] std::optional<std::uint64_t>
parse_uint64_literal(std::string_view text) noexcept {
    if (text.empty()) {
        return std::nullopt;
    }
    if (text.front() == '-') {
        return std::nullopt;
    }

    int base = 10;
    if (text.size() > 2 && text[0] == '0' &&
        (text[1] == 'x' || text[1] == 'X')) {
        base = 16;
        text.remove_prefix(2);
    }
    if (text.empty()) {
        return std::nullopt;
    }

    std::uint64_t value = 0;
    auto [ptr, ec] =
        std::from_chars(text.data(), text.data() + text.size(), value, base);
    if (ec != std::errc{} || ptr != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] std::optional<std::int64_t>
parse_int64_literal(std::string_view text) noexcept {
    if (text.empty()) {
        return std::nullopt;
    }

    bool negative = false;
    if (text.front() == '-') {
        negative = true;
        text.remove_prefix(1);
    }

    const auto mag = parse_uint64_literal(text);
    if (!mag.has_value()) {
        return std::nullopt;
    }

    const auto magnitude = *mag;
    if (!negative) {
        if (magnitude > static_cast<std::uint64_t>(
                            std::numeric_limits<std::int64_t>::max())) {
            return std::nullopt;
        }
        return static_cast<std::int64_t>(magnitude);
    }

    const auto max_plus_one =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) +
        1u;
    if (magnitude > max_plus_one) {
        return std::nullopt;
    }
    if (magnitude == max_plus_one) {
        return std::numeric_limits<std::int64_t>::min();
    }
    return -static_cast<std::int64_t>(magnitude);
}

double parse_float_value(std::string_view text) {
    // token.value 来自 std::string，按 C++20 约定以 '\0' 结尾，可直接交给
    // strtod。
    return std::strtod(text.data(), nullptr);
}

template <class T>
[[nodiscard]] bool values_has_var(
    const std::vector<ValueExpr<T>> &values) noexcept {
    for (const auto &v : values) {
        if (std::holds_alternative<VarRef>(v)) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool item_has_var(const TemplateItem &item) noexcept {
    struct Visitor final {
        static bool run(const TemplateItem &it) noexcept {
            return std::visit(Visitor{}, it.storage());
        }

        bool operator()(const TplList &list) const noexcept {
            for (const auto &child : list) {
                if (run(child)) {
                    return true;
                }
            }
            return false;
        }

        bool operator()(const TplASCII &a) const noexcept {
            return std::holds_alternative<VarRef>(a.value);
        }

        bool operator()(const TplBinary &b) const noexcept {
            return values_has_var(b.values);
        }

        bool operator()(const TplBoolean &b) const noexcept {
            return values_has_var(b.values);
        }

        bool operator()(const TplI1 &v) const noexcept {
            return values_has_var(v.values);
        }
        bool operator()(const TplI2 &v) const noexcept {
            return values_has_var(v.values);
        }
        bool operator()(const TplI4 &v) const noexcept {
            return values_has_var(v.values);
        }
        bool operator()(const TplI8 &v) const noexcept {
            return values_has_var(v.values);
        }

        bool operator()(const TplU1 &v) const noexcept {
            return values_has_var(v.values);
        }
        bool operator()(const TplU2 &v) const noexcept {
            return values_has_var(v.values);
        }
        bool operator()(const TplU4 &v) const noexcept {
            return values_has_var(v.values);
        }
        bool operator()(const TplU8 &v) const noexcept {
            return values_has_var(v.values);
        }

        bool operator()(const TplF4 &v) const noexcept {
            return values_has_var(v.values);
        }
        bool operator()(const TplF8 &v) const noexcept {
            return values_has_var(v.values);
        }
    };

    return Visitor::run(item);
}

template <class T>
[[nodiscard]] std::optional<std::vector<T>>
values_to_literals(const std::vector<ValueExpr<T>> &values) noexcept {
    std::vector<T> out;
    out.reserve(values.size());
    for (const auto &v : values) {
        const auto *lit = std::get_if<T>(&v);
        if (!lit) {
            return std::nullopt;
        }
        out.push_back(*lit);
    }
    return out;
}

[[nodiscard]] std::optional<secs::ii::Item>
tpl_to_ii_item(const TemplateItem &item) noexcept {
    struct Visitor final {
        static std::optional<secs::ii::Item> run(const TemplateItem &it) noexcept {
            return std::visit(Visitor{}, it.storage());
        }

        std::optional<secs::ii::Item> operator()(const TplList &list) const noexcept {
            std::vector<secs::ii::Item> out;
            out.reserve(list.size());
            for (const auto &child : list) {
                auto c = run(child);
                if (!c.has_value()) {
                    return std::nullopt;
                }
                out.push_back(std::move(*c));
            }
            return secs::ii::Item::list(std::move(out));
        }

        std::optional<secs::ii::Item> operator()(const TplASCII &a) const noexcept {
            const auto *s = std::get_if<std::string>(&a.value);
            if (!s) {
                return std::nullopt;
            }
            return secs::ii::Item::ascii(*s);
        }

        std::optional<secs::ii::Item> operator()(const TplBinary &b) const noexcept {
            auto bytes = values_to_literals<secs::ii::byte>(b.values);
            if (!bytes.has_value()) {
                return std::nullopt;
            }
            return secs::ii::Item::binary(std::move(*bytes));
        }

        std::optional<secs::ii::Item> operator()(const TplBoolean &b) const noexcept {
            auto values = values_to_literals<bool>(b.values);
            if (!values.has_value()) {
                return std::nullopt;
            }
            return secs::ii::Item::boolean(std::move(*values));
        }

        std::optional<secs::ii::Item> operator()(const TplI1 &v) const noexcept {
            auto values = values_to_literals<std::int8_t>(v.values);
            if (!values.has_value()) {
                return std::nullopt;
            }
            return secs::ii::Item::i1(std::move(*values));
        }
        std::optional<secs::ii::Item> operator()(const TplI2 &v) const noexcept {
            auto values = values_to_literals<std::int16_t>(v.values);
            if (!values.has_value()) {
                return std::nullopt;
            }
            return secs::ii::Item::i2(std::move(*values));
        }
        std::optional<secs::ii::Item> operator()(const TplI4 &v) const noexcept {
            auto values = values_to_literals<std::int32_t>(v.values);
            if (!values.has_value()) {
                return std::nullopt;
            }
            return secs::ii::Item::i4(std::move(*values));
        }
        std::optional<secs::ii::Item> operator()(const TplI8 &v) const noexcept {
            auto values = values_to_literals<std::int64_t>(v.values);
            if (!values.has_value()) {
                return std::nullopt;
            }
            return secs::ii::Item::i8(std::move(*values));
        }

        std::optional<secs::ii::Item> operator()(const TplU1 &v) const noexcept {
            auto values = values_to_literals<std::uint8_t>(v.values);
            if (!values.has_value()) {
                return std::nullopt;
            }
            return secs::ii::Item::u1(std::move(*values));
        }
        std::optional<secs::ii::Item> operator()(const TplU2 &v) const noexcept {
            auto values = values_to_literals<std::uint16_t>(v.values);
            if (!values.has_value()) {
                return std::nullopt;
            }
            return secs::ii::Item::u2(std::move(*values));
        }
        std::optional<secs::ii::Item> operator()(const TplU4 &v) const noexcept {
            auto values = values_to_literals<std::uint32_t>(v.values);
            if (!values.has_value()) {
                return std::nullopt;
            }
            return secs::ii::Item::u4(std::move(*values));
        }
        std::optional<secs::ii::Item> operator()(const TplU8 &v) const noexcept {
            auto values = values_to_literals<std::uint64_t>(v.values);
            if (!values.has_value()) {
                return std::nullopt;
            }
            return secs::ii::Item::u8(std::move(*values));
        }

        std::optional<secs::ii::Item> operator()(const TplF4 &v) const noexcept {
            auto values = values_to_literals<float>(v.values);
            if (!values.has_value()) {
                return std::nullopt;
            }
            return secs::ii::Item::f4(std::move(*values));
        }
        std::optional<secs::ii::Item> operator()(const TplF8 &v) const noexcept {
            auto values = values_to_literals<double>(v.values);
            if (!values.has_value()) {
                return std::nullopt;
            }
            return secs::ii::Item::f8(std::move(*values));
        }
    };

    return Visitor::run(item);
}

} // namespace

const std::error_category &parser_error_category() noexcept {
    return kParserErrorCategory;
}

std::error_code make_error_code(parser_errc e) noexcept {
    return {static_cast<int>(e), kParserErrorCategory};
}

Parser::Parser(std::vector<Token> tokens) noexcept
    : tokens_(std::move(tokens)) {}

ParseResult Parser::parse() noexcept {
    while (!at_end() && !had_error_) {
        parse_statement();
    }

    ParseResult result;
    result.document = std::move(document_);
    result.ec = ec_;
    result.error_line = error_line_;
    result.error_column = error_column_;
    result.error_message = std::move(error_message_);
    return result;
}

bool Parser::at_end() const noexcept { return peek().type == TokenType::Eof; }

const Token &Parser::peek() const noexcept { return tokens_[current_]; }

const Token &Parser::previous() const noexcept { return tokens_[current_ - 1]; }

const Token &Parser::advance() noexcept {
    if (!at_end())
        ++current_;
    return previous();
}

bool Parser::check(TokenType type) const noexcept {
    return peek().type == type;
}

bool Parser::match(TokenType type) noexcept {
    if (check(type)) {
        advance();
        return true;
    }
    return false;
}

bool Parser::parse_statement() noexcept {
    // 条件规则：以关键字 "if" 开始
    if (match(TokenType::KwIf)) {
        return parse_if_rule();
    }

    // 定时发送规则：以关键字 "every" 开始
    if (match(TokenType::KwEvery)) {
        return parse_every_rule();
    }

    // 消息定义
    return parse_message_def();
}

bool Parser::parse_message_def() noexcept {
    MessageDef msg;

    // 可能的格式:
    // 1. 命名消息：name: SxFy [W] <Item>.
    // 2. 命名消息：name: 'SxFy' [W] <Item>.
    // 3. 匿名消息：SxFy [W] <Item>.
    // 4. 匿名消息：SxFy.（无消息体）

    std::string first_token;

    if (!check(TokenType::Identifier) && !check(TokenType::LAngle)) {
        error("expected message definition");
        return false;
    }

    if (check(TokenType::Identifier)) {
        first_token = peek().value;
        advance();

        // 检查是否有冒号 (命名消息)
        if (match(TokenType::Colon)) {
            msg.name = first_token;

            // 获取 SxFy
            if (check(TokenType::Identifier)) {
                first_token = peek().value;
                advance();
            } else if (check(TokenType::String)) {
                first_token = peek().value;
                advance();
            } else {
                error("expected stream/function after ':'");
                return false;
            }
        }
    }

    // 解析 SxFy
    if (!parse_sf_string(first_token, msg.stream, msg.function)) {
        error(parser_errc::invalid_stream_function,
              "invalid stream/function format: " + first_token);
        return false;
    }

    // 可选的 W
    if (match(TokenType::KwW)) {
        msg.w_bit = true;
    }

    // 可选的 Item
    if (check(TokenType::LAngle)) {
        auto item = parse_item();
        if (!item) {
            return false;
        }
        msg.item = std::move(*item);
    }

    // 结束点
    if (!match(TokenType::Dot)) {
        error("expected '.' at end of message definition");
        return false;
    }

    document_.messages.push_back(std::move(msg));
    return true;
}

bool Parser::parse_if_rule() noexcept {
    // 语法：if (条件) 响应消息名.
    if (!match(TokenType::LParen)) {
        error("expected '(' after 'if'");
        return false;
    }

    auto cond = parse_condition();
    if (!cond) {
        return false;
    }

    if (!match(TokenType::RParen)) {
        error("expected ')' after condition");
        return false;
    }

    // 响应消息名
    if (!check(TokenType::Identifier)) {
        error("expected response message name");
        return false;
    }

    ConditionRule rule;
    rule.condition = std::move(*cond);
    rule.response_name = advance().value;

    if (!match(TokenType::Dot)) {
        error("expected '.' at end of if rule");
        return false;
    }

    document_.conditions.push_back(std::move(rule));
    return true;
}

bool Parser::parse_every_rule() noexcept {
    // 语法：every N send 消息名.
    if (!check(TokenType::Integer)) {
        error(parser_errc::expected_number, "expected interval after 'every'");
        return false;
    }

    TimerRule rule;
    {
        const auto v = parse_int64_literal(advance().value);
        if (!v.has_value() || *v < 0 ||
            *v > static_cast<std::int64_t>(
                     std::numeric_limits<std::uint32_t>::max())) {
            error(parser_errc::expected_number, "interval out of range");
            return false;
        }
        rule.interval_seconds = static_cast<std::uint32_t>(*v);
    }

    if (!match(TokenType::KwSend)) {
        error("expected 'send' after interval");
        return false;
    }

    if (!check(TokenType::Identifier)) {
        error("expected message name after 'send'");
        return false;
    }

    rule.message_name = advance().value;

    if (!match(TokenType::Dot)) {
        error("expected '.' at end of every rule");
        return false;
    }

    document_.timers.push_back(std::move(rule));
    return true;
}

std::optional<TemplateItem> Parser::parse_item() noexcept {
    if (!match(TokenType::LAngle)) {
        error(parser_errc::expected_item, "expected '<'");
        return std::nullopt;
    }

    std::optional<TemplateItem> result;

    TokenType type = peek().type;
    switch (type) {
    case TokenType::KwL:
        result = parse_list();
        break;
    case TokenType::KwA:
        result = parse_ascii();
        break;
    case TokenType::KwB:
        result = parse_binary();
        break;
    case TokenType::KwBoolean:
        result = parse_boolean();
        break;
    case TokenType::KwU1:
    case TokenType::KwU2:
    case TokenType::KwU4:
    case TokenType::KwU8:
        result = parse_unsigned(type);
        break;
    case TokenType::KwI1:
    case TokenType::KwI2:
    case TokenType::KwI4:
    case TokenType::KwI8:
        result = parse_signed(type);
        break;
    case TokenType::KwF4:
    case TokenType::KwF8:
        result = parse_float(type);
        break;
    default:
        error(parser_errc::expected_item, "expected item type");
        return std::nullopt;
    }

    if (!result) {
        return std::nullopt;
    }

    if (!match(TokenType::RAngle)) {
        error(parser_errc::unclosed_item, "expected '>'");
        return std::nullopt;
    }

    return result;
}

std::optional<TemplateItem> Parser::parse_list() noexcept {
    advance(); // L（列表）

    // 可选的 [n] 大小提示
    if (match(TokenType::LBracket)) {
        // 跳过大小提示
        while (!check(TokenType::RBracket) && !at_end()) {
            advance();
        }
        if (!match(TokenType::RBracket)) {
            error("expected ']'");
            return std::nullopt;
        }
    }

    TplList items;
    while (check(TokenType::LAngle)) {
        auto item = parse_item();
        if (!item) {
            return std::nullopt;
        }
        items.push_back(std::move(*item));
    }

    return TemplateItem(std::move(items));
}

std::optional<TemplateItem> Parser::parse_ascii() noexcept {
    advance(); // A（ASCII 字符串）

    TplASCII a;
    if (check(TokenType::String)) {
        a.value = advance().value;
        return TemplateItem(std::move(a));
    }
    if (check(TokenType::Identifier)) {
        a.value = VarRef{advance().value};
        return TemplateItem(std::move(a));
    }

    // 空 ASCII
    a.value = std::string{};
    return TemplateItem(std::move(a));
}

std::optional<TemplateItem> Parser::parse_binary() noexcept {
    advance(); // B（二进制数组）

    TplBinary b;
    while (check(TokenType::Integer) || check(TokenType::Identifier)) {
        if (check(TokenType::Identifier)) {
            b.values.emplace_back(VarRef{advance().value});
            continue;
        }

        const auto val = parse_uint64_literal(advance().value);
        if (!val.has_value() || *val > 0xFFu) {
            error(parser_errc::expected_number,
                  "binary byte out of range (expected 0..255)");
            return std::nullopt;
        }
        b.values.emplace_back(static_cast<secs::ii::byte>(*val));
    }

    return TemplateItem(std::move(b));
}

std::optional<TemplateItem> Parser::parse_boolean() noexcept {
    advance(); // Boolean（布尔数组）

    TplBoolean b;
    while (check(TokenType::Integer) || check(TokenType::Identifier)) {
        if (check(TokenType::Identifier)) {
            b.values.emplace_back(VarRef{advance().value});
            continue;
        }

        const auto val = parse_int64_literal(advance().value);
        if (!val.has_value()) {
            error(parser_errc::expected_number, "invalid boolean literal");
            return std::nullopt;
        }
        b.values.emplace_back(*val != 0);
    }

    return TemplateItem(std::move(b));
}

std::optional<TemplateItem> Parser::parse_unsigned(TokenType type) noexcept {
    advance(); // U1/U2/U4/U8（无符号整数数组）

    switch (type) {
    case TokenType::KwU1: {
        TplU1 v;
        while (check(TokenType::Integer) || check(TokenType::Identifier)) {
            if (check(TokenType::Identifier)) {
                v.values.emplace_back(VarRef{advance().value});
                continue;
            }

            const auto n = parse_uint64_literal(advance().value);
            if (!n.has_value() ||
                *n > std::numeric_limits<std::uint8_t>::max()) {
                error(parser_errc::expected_number, "U1 value out of range");
                return std::nullopt;
            }
            v.values.emplace_back(static_cast<std::uint8_t>(*n));
        }
        return TemplateItem(std::move(v));
    }
    case TokenType::KwU2: {
        TplU2 v;
        while (check(TokenType::Integer) || check(TokenType::Identifier)) {
            if (check(TokenType::Identifier)) {
                v.values.emplace_back(VarRef{advance().value});
                continue;
            }

            const auto n = parse_uint64_literal(advance().value);
            if (!n.has_value() ||
                *n > std::numeric_limits<std::uint16_t>::max()) {
                error(parser_errc::expected_number, "U2 value out of range");
                return std::nullopt;
            }
            v.values.emplace_back(static_cast<std::uint16_t>(*n));
        }
        return TemplateItem(std::move(v));
    }
    case TokenType::KwU4: {
        TplU4 v;
        while (check(TokenType::Integer) || check(TokenType::Identifier)) {
            if (check(TokenType::Identifier)) {
                v.values.emplace_back(VarRef{advance().value});
                continue;
            }

            const auto n = parse_uint64_literal(advance().value);
            if (!n.has_value() ||
                *n > std::numeric_limits<std::uint32_t>::max()) {
                error(parser_errc::expected_number, "U4 value out of range");
                return std::nullopt;
            }
            v.values.emplace_back(static_cast<std::uint32_t>(*n));
        }
        return TemplateItem(std::move(v));
    }
    case TokenType::KwU8: {
        TplU8 v;
        while (check(TokenType::Integer) || check(TokenType::Identifier)) {
            if (check(TokenType::Identifier)) {
                v.values.emplace_back(VarRef{advance().value});
                continue;
            }

            const auto n = parse_uint64_literal(advance().value);
            if (!n.has_value()) {
                error(parser_errc::expected_number, "U8 value out of range");
                return std::nullopt;
            }
            v.values.emplace_back(*n);
        }
        return TemplateItem(std::move(v));
    }
    default:
        return std::nullopt;
    }
}

std::optional<TemplateItem> Parser::parse_signed(TokenType type) noexcept {
    advance(); // I1/I2/I4/I8（有符号整数数组）

    switch (type) {
    case TokenType::KwI1: {
        TplI1 v;
        while (check(TokenType::Integer) || check(TokenType::Identifier)) {
            if (check(TokenType::Identifier)) {
                v.values.emplace_back(VarRef{advance().value});
                continue;
            }

            const auto n = parse_int64_literal(advance().value);
            if (!n.has_value() ||
                *n < std::numeric_limits<std::int8_t>::min() ||
                *n > std::numeric_limits<std::int8_t>::max()) {
                error(parser_errc::expected_number, "I1 value out of range");
                return std::nullopt;
            }
            v.values.emplace_back(static_cast<std::int8_t>(*n));
        }
        return TemplateItem(std::move(v));
    }
    case TokenType::KwI2: {
        TplI2 v;
        while (check(TokenType::Integer) || check(TokenType::Identifier)) {
            if (check(TokenType::Identifier)) {
                v.values.emplace_back(VarRef{advance().value});
                continue;
            }

            const auto n = parse_int64_literal(advance().value);
            if (!n.has_value() ||
                *n < std::numeric_limits<std::int16_t>::min() ||
                *n > std::numeric_limits<std::int16_t>::max()) {
                error(parser_errc::expected_number, "I2 value out of range");
                return std::nullopt;
            }
            v.values.emplace_back(static_cast<std::int16_t>(*n));
        }
        return TemplateItem(std::move(v));
    }
    case TokenType::KwI4: {
        TplI4 v;
        while (check(TokenType::Integer) || check(TokenType::Identifier)) {
            if (check(TokenType::Identifier)) {
                v.values.emplace_back(VarRef{advance().value});
                continue;
            }

            const auto n = parse_int64_literal(advance().value);
            if (!n.has_value() ||
                *n < std::numeric_limits<std::int32_t>::min() ||
                *n > std::numeric_limits<std::int32_t>::max()) {
                error(parser_errc::expected_number, "I4 value out of range");
                return std::nullopt;
            }
            v.values.emplace_back(static_cast<std::int32_t>(*n));
        }
        return TemplateItem(std::move(v));
    }
    case TokenType::KwI8: {
        TplI8 v;
        while (check(TokenType::Integer) || check(TokenType::Identifier)) {
            if (check(TokenType::Identifier)) {
                v.values.emplace_back(VarRef{advance().value});
                continue;
            }

            const auto n = parse_int64_literal(advance().value);
            if (!n.has_value()) {
                error(parser_errc::expected_number, "I8 value out of range");
                return std::nullopt;
            }
            v.values.emplace_back(*n);
        }
        return TemplateItem(std::move(v));
    }
    default:
        return std::nullopt;
    }
}

std::optional<TemplateItem> Parser::parse_float(TokenType type) noexcept {
    advance(); // F4/F8（浮点数组）

    if (type == TokenType::KwF4) {
        TplF4 v;
        while (check(TokenType::Float) || check(TokenType::Integer) ||
               check(TokenType::Identifier)) {
            if (check(TokenType::Identifier)) {
                v.values.emplace_back(VarRef{advance().value});
                continue;
            }
            v.values.emplace_back(
                static_cast<float>(parse_float_value(advance().value)));
        }
        return TemplateItem(std::move(v));
    } else {
        TplF8 v;
        while (check(TokenType::Float) || check(TokenType::Integer) ||
               check(TokenType::Identifier)) {
            if (check(TokenType::Identifier)) {
                v.values.emplace_back(VarRef{advance().value});
                continue;
            }
            v.values.emplace_back(parse_float_value(advance().value));
        }
        return TemplateItem(std::move(v));
    }
}

std::optional<Condition> Parser::parse_condition() noexcept {
    // 语法：消息名 [(索引)][==<Item>]
    Condition cond;

    if (!check(TokenType::Identifier)) {
        error(parser_errc::invalid_condition,
              "expected message name in condition");
        return std::nullopt;
    }

    cond.message_name = advance().value;

    // 可选的 (索引)
    if (match(TokenType::LParen)) {
        if (!check(TokenType::Integer)) {
            error(parser_errc::expected_number, "expected index number");
            return std::nullopt;
        }
        const auto idx = parse_uint64_literal(advance().value);
        if (!idx.has_value() ||
            *idx > std::numeric_limits<std::size_t>::max()) {
            error(parser_errc::expected_number, "index out of range");
            return std::nullopt;
        }
        if (*idx < 1) {
            error(parser_errc::invalid_condition, "index must be >= 1");
            return std::nullopt;
        }
        cond.index = static_cast<std::size_t>(*idx);
        if (!match(TokenType::RParen)) {
            error(parser_errc::invalid_condition, "expected ')' after index");
            return std::nullopt;
        }
    }

    // 可选的 ==<Item>（期望值匹配）
    if (match(TokenType::Equals)) {
        auto tpl = parse_item();
        if (!tpl) {
            return std::nullopt;
        }

        // 约束：条件期望值当前仅支持“纯字面量”，不允许占位符（避免在匹配阶段
        // 引入隐式上下文依赖）。
        if (item_has_var(*tpl)) {
            error(parser_errc::invalid_condition,
                  "placeholders are not allowed in expected item");
            return std::nullopt;
        }

        auto expected = tpl_to_ii_item(*tpl);
        if (!expected.has_value()) {
            error(parser_errc::invalid_condition, "invalid expected item");
            return std::nullopt;
        }
        cond.expected = std::move(*expected);
    }

    return cond;
}

void Parser::error(std::string_view message) noexcept {
    error(parser_errc::unexpected_token, message);
}

void Parser::error_at(const Token &token, std::string_view message) noexcept {
    error_at(parser_errc::unexpected_token, token, message);
}

void Parser::error(parser_errc code, std::string_view message) noexcept {
    error_at(code, peek(), message);
}

void Parser::error_at(parser_errc code,
                      const Token &token,
                      std::string_view message) noexcept {
    if (had_error_)
        return;

    had_error_ = true;
    ec_ = make_error_code(code);
    error_line_ = token.line;
    error_column_ = token.column;
    error_message_ = std::string(message) + " at '" + token.value + "'";
}

} // namespace secs::sml
