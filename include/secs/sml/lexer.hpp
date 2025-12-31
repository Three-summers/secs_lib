#pragma once

#include "secs/sml/token.hpp"

#include <string_view>
#include <system_error>
#include <vector>

namespace secs::sml {

enum class lexer_errc : int {
  ok = 0,
  unterminated_string = 1,
  unterminated_comment = 2,
  invalid_character = 3,
  invalid_hex_literal = 4,
};

const std::error_category& lexer_error_category() noexcept;
std::error_code make_error_code(lexer_errc e) noexcept;

struct LexerResult {
  std::vector<Token> tokens;
  std::error_code ec;
  std::uint32_t error_line{0};
  std::uint32_t error_column{0};
  std::string error_message;
};

/**
 * @brief SML 词法分析器
 *
 * 将 SML 源文本转换为 Token 序列。
 * 支持:
 * - 标识符: name, S1F1, StatusTank1
 * - 字符串: "..." 或 '...'
 * - 数字: 123, 0x1F, 0.5567
 * - 关键字: if, every, send, W, L, A, B, Boolean, U1-U8, I1-I8, F4, F8
 * - 注释: 块注释和行注释
 */
class Lexer {
 public:
  explicit Lexer(std::string_view source) noexcept;

  [[nodiscard]] LexerResult tokenize() noexcept;

 private:
  [[nodiscard]] bool at_end() const noexcept;
  [[nodiscard]] char peek() const noexcept;
  [[nodiscard]] char peek_next() const noexcept;
  char advance() noexcept;
  void skip_whitespace() noexcept;
  bool skip_comment(LexerResult& result) noexcept;

  Token scan_token() noexcept;
  Token scan_identifier() noexcept;
  Token scan_string(char quote) noexcept;
  Token scan_number() noexcept;

  Token make_token(TokenType type) const noexcept;
  Token make_token(TokenType type, std::string value) const noexcept;
  Token make_error(lexer_errc kind, std::string_view message) noexcept;
  Token make_error(std::string_view message) noexcept { return make_error(lexer_errc::invalid_character, message); }

  [[nodiscard]] TokenType identifier_type(std::string_view text) const noexcept;

  std::string_view source_;
  std::size_t current_{0};
  std::size_t token_start_{0};
  std::uint32_t line_{1};
  std::uint32_t column_{1};
  std::uint32_t token_line_{1};
  std::uint32_t token_column_{1};
  lexer_errc last_error_kind_{lexer_errc::invalid_character};
};

}  // 命名空间 secs::sml

namespace std {
template <>
struct is_error_code_enum<secs::sml::lexer_errc> : true_type {};
}  // 命名空间 std
