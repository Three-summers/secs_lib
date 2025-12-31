#include "secs/sml/lexer.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <unordered_map>

namespace secs::sml {

namespace {

class LexerErrorCategory : public std::error_category {
 public:
  [[nodiscard]] const char* name() const noexcept override { return "sml.lexer"; }

  [[nodiscard]] std::string message(int ev) const override {
    switch (static_cast<lexer_errc>(ev)) {
      case lexer_errc::ok: return "success";
      case lexer_errc::unterminated_string: return "unterminated string literal";
      case lexer_errc::unterminated_comment: return "unterminated block comment";
      case lexer_errc::invalid_character: return "invalid character";
      case lexer_errc::invalid_hex_literal: return "invalid hexadecimal literal";
    }
    return "unknown lexer error";
  }
};

const LexerErrorCategory kLexerErrorCategory{};

const std::unordered_map<std::string_view, TokenType> kKeywords = {
    {"if", TokenType::KwIf},
    {"every", TokenType::KwEvery},
    {"send", TokenType::KwSend},
    {"W", TokenType::KwW},
    {"L", TokenType::KwL},
    {"A", TokenType::KwA},
    {"B", TokenType::KwB},
    {"Boolean", TokenType::KwBoolean},
    {"U1", TokenType::KwU1},
    {"U2", TokenType::KwU2},
    {"U4", TokenType::KwU4},
    {"U8", TokenType::KwU8},
    {"I1", TokenType::KwI1},
    {"I2", TokenType::KwI2},
    {"I4", TokenType::KwI4},
    {"I8", TokenType::KwI8},
    {"F4", TokenType::KwF4},
    {"F8", TokenType::KwF8},
};

}  // namespace

const std::error_category& lexer_error_category() noexcept {
  return kLexerErrorCategory;
}

std::error_code make_error_code(lexer_errc e) noexcept {
  return {static_cast<int>(e), kLexerErrorCategory};
}

Lexer::Lexer(std::string_view source) noexcept : source_(source) {}

LexerResult Lexer::tokenize() noexcept {
  LexerResult result;

  while (!at_end()) {
    skip_whitespace();
    if (at_end()) break;

    // 跳过注释
    if (skip_comment()) {
      continue;
    }

    token_start_ = current_;
    token_line_ = line_;
    token_column_ = column_;

    Token token = scan_token();
    if (token.type == TokenType::Error) {
      result.ec = make_error_code(lexer_errc::invalid_character);
      result.error_line = token.line;
      result.error_column = token.column;
      result.error_message = token.value;
      return result;
    }

    result.tokens.push_back(std::move(token));
  }

  result.tokens.push_back(make_token(TokenType::Eof));
  return result;
}

bool Lexer::at_end() const noexcept {
  return current_ >= source_.size();
}

char Lexer::peek() const noexcept {
  if (at_end()) return '\0';
  return source_[current_];
}

char Lexer::peek_next() const noexcept {
  if (current_ + 1 >= source_.size()) return '\0';
  return source_[current_ + 1];
}

char Lexer::advance() noexcept {
  char c = source_[current_++];
  if (c == '\n') {
    ++line_;
    column_ = 1;
  } else {
    ++column_;
  }
  return c;
}

void Lexer::skip_whitespace() noexcept {
  while (!at_end()) {
    char c = peek();
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      advance();
    } else {
      break;
    }
  }
}

bool Lexer::skip_comment() noexcept {
  if (peek() == '/' && peek_next() == '*') {
    // 块注释：/* ... */
    advance();  // /
    advance();  // *
    while (!at_end()) {
      if (peek() == '*' && peek_next() == '/') {
        advance();  // *
        advance();  // /
        return true;
      }
      advance();
    }
    // 未闭合的块注释：当前实现会直接读到 EOF 并结束（不额外生成错误记号）。
    return true;
  }

  if (peek() == '/' && peek_next() == '/') {
    // 行注释：// ...
    while (!at_end() && peek() != '\n') {
      advance();
    }
    return true;
  }

  return false;
}

Token Lexer::scan_token() noexcept {
  char c = advance();

  // 单字符记号（标点等）
  switch (c) {
    case ':': return make_token(TokenType::Colon);
    case '.': return make_token(TokenType::Dot);
    case '<': return make_token(TokenType::LAngle);
    case '>': return make_token(TokenType::RAngle);
    case '(': return make_token(TokenType::LParen);
    case ')': return make_token(TokenType::RParen);
    case '[': return make_token(TokenType::LBracket);
    case ']': return make_token(TokenType::RBracket);
    case '=':
      if (peek() == '=') {
        advance();
        return make_token(TokenType::Equals);
      }
      return make_error("unexpected '=', expected '=='");
    case '"':
    case '\'':
      return scan_string(c);
    default:
      break;
  }

  // 标识符或关键字
  if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
    --current_;
    --column_;
    return scan_identifier();
  }

  // 数字：支持十进制整数、十六进制整数（0x..）与浮点数（含科学计数法）
  if (std::isdigit(static_cast<unsigned char>(c)) || (c == '-' && std::isdigit(static_cast<unsigned char>(peek())))) {
    --current_;
    --column_;
    return scan_number();
  }

  return make_error(std::string("unexpected character: ") + c);
}

Token Lexer::scan_identifier() noexcept {
  std::size_t start = current_;
  while (!at_end() && (std::isalnum(static_cast<unsigned char>(peek())) || peek() == '_')) {
    advance();
  }

  std::string_view text = source_.substr(start, current_ - start);
  TokenType type = identifier_type(text);
  return make_token(type, std::string(text));
}

Token Lexer::scan_string(char quote) noexcept {
  std::string value;
  while (!at_end() && peek() != quote) {
    if (peek() == '\n') {
      return make_error("unterminated string (newline in string)");
    }
    if (peek() == '\\' && peek_next() != '\0') {
      advance();  // 反斜杠
      char escaped = advance();
      switch (escaped) {
        case 'n': value += '\n'; break;
        case 't': value += '\t'; break;
        case 'r': value += '\r'; break;
        case '\\': value += '\\'; break;
        case '"': value += '"'; break;
        case '\'': value += '\''; break;
        default: value += escaped; break;
      }
    } else {
      value += advance();
    }
  }

  if (at_end()) {
    return make_error("unterminated string");
  }

  advance();  // 结束引号
  return make_token(TokenType::String, std::move(value));
}

Token Lexer::scan_number() noexcept {
  std::size_t start = current_;

  if (peek() == '-') {
    advance();
  }

  // 判断十六进制：0x1F / 0X1F
  if (peek() == '0' && (peek_next() == 'x' || peek_next() == 'X')) {
    advance();  // 0
    advance();  // x
    while (!at_end() && std::isxdigit(static_cast<unsigned char>(peek()))) {
      advance();
    }
    std::string_view text = source_.substr(start, current_ - start);
    return make_token(TokenType::Integer, std::string(text));
  }

  // 整数部分
  while (!at_end() && std::isdigit(static_cast<unsigned char>(peek()))) {
    advance();
  }

  // 判断是否为浮点数：要求 '.' 后至少有一位数字，避免把 "S1F1." 的 '.' 误吞成小数点
  if (peek() == '.' && std::isdigit(static_cast<unsigned char>(peek_next()))) {
    advance();  // .
    while (!at_end() && std::isdigit(static_cast<unsigned char>(peek()))) {
      advance();
    }
    // 指数部分：e/E[+/-]后跟数字
    if (peek() == 'e' || peek() == 'E') {
      advance();
      if (peek() == '+' || peek() == '-') {
        advance();
      }
      while (!at_end() && std::isdigit(static_cast<unsigned char>(peek()))) {
        advance();
      }
    }
    std::string_view text = source_.substr(start, current_ - start);
    return make_token(TokenType::Float, std::string(text));
  }

  std::string_view text = source_.substr(start, current_ - start);
  return make_token(TokenType::Integer, std::string(text));
}

Token Lexer::make_token(TokenType type) const noexcept {
  return Token{type, std::string(source_.substr(token_start_, current_ - token_start_)),
               token_line_, token_column_};
}

Token Lexer::make_token(TokenType type, std::string value) const noexcept {
  return Token{type, std::move(value), token_line_, token_column_};
}

Token Lexer::make_error(std::string_view message) const noexcept {
  return Token{TokenType::Error, std::string(message), token_line_, token_column_};
}

TokenType Lexer::identifier_type(std::string_view text) const noexcept {
  auto it = kKeywords.find(text);
  if (it != kKeywords.end()) {
    return it->second;
  }
  return TokenType::Identifier;
}

}  // namespace secs::sml
