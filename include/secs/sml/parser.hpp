#pragma once

#include "secs/sml/ast.hpp"
#include "secs/sml/token.hpp"

#include <string_view>
#include <system_error>
#include <vector>

namespace secs::sml {

enum class parser_errc : int {
    ok = 0,
    unexpected_token = 1,
    expected_item = 2,
    expected_identifier = 3,
    expected_number = 4,
    invalid_stream_function = 5,
    unclosed_item = 6,
    invalid_condition = 7,
};

const std::error_category &parser_error_category() noexcept;
std::error_code make_error_code(parser_errc e) noexcept;

struct ParseResult {
    Document document;
    std::error_code ec;
    std::uint32_t error_line{0};
    std::uint32_t error_column{0};
    std::string error_message;
};

/**
 * @brief SML 语法分析器
 *
 * 将 Token 序列转换为 AST (Document)。
 * 使用递归下降解析。
 */
class Parser {
public:
    explicit Parser(std::vector<Token> tokens) noexcept;

    [[nodiscard]] ParseResult parse() noexcept;

private:
    [[nodiscard]] bool at_end() const noexcept;
    [[nodiscard]] const Token &peek() const noexcept;
    [[nodiscard]] const Token &previous() const noexcept;
    const Token &advance() noexcept;
    bool check(TokenType type) const noexcept;
    bool match(TokenType type) noexcept;

    // 解析规则
    bool parse_statement() noexcept;
    bool parse_message_def() noexcept;
    bool parse_if_rule() noexcept;
    bool parse_every_rule() noexcept;

    // 解析 SECS-II 数据项（<...> 语法）
    std::optional<ii::Item> parse_item() noexcept;
    std::optional<ii::Item> parse_list() noexcept;
    std::optional<ii::Item> parse_ascii() noexcept;
    std::optional<ii::Item> parse_binary() noexcept;
    std::optional<ii::Item> parse_boolean() noexcept;
    std::optional<ii::Item> parse_unsigned(TokenType type) noexcept;
    std::optional<ii::Item> parse_signed(TokenType type) noexcept;
    std::optional<ii::Item> parse_float(TokenType type) noexcept;

    // 解析辅助
    std::optional<Condition> parse_condition() noexcept;

    // 错误处理
    void error(parser_errc code, std::string_view message) noexcept;
    void error(std::string_view message) noexcept;
    void error_at(parser_errc code,
                  const Token &token,
                  std::string_view message) noexcept;
    void error_at(const Token &token, std::string_view message) noexcept;

    std::vector<Token> tokens_;
    std::size_t current_{0};

    Document document_;
    std::error_code ec_;
    std::uint32_t error_line_{0};
    std::uint32_t error_column_{0};
    std::string error_message_;
    bool had_error_{false};
};

} // namespace secs::sml

namespace std {
template <>
struct is_error_code_enum<secs::sml::parser_errc> : true_type {};
} // namespace std
