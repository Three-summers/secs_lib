#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace secs::sml {

enum class TokenType : std::uint8_t {
  // Literals
  Identifier,     // name, S1F1, etc.
  String,         // "..." or '...'
  Integer,        // 123, 0x1F
  Float,          // 0.5567

  // Keywords
  KwIf,           // if
  KwEvery,        // every
  KwSend,         // send
  KwW,            // W (wait bit)

  // Item types
  KwL,            // L (List)
  KwA,            // A (ASCII)
  KwB,            // B (Binary)
  KwBoolean,      // Boolean
  KwU1, KwU2, KwU4, KwU8,  // Unsigned
  KwI1, KwI2, KwI4, KwI8,  // Signed
  KwF4, KwF8,     // Float

  // Punctuation
  Colon,          // :
  Dot,            // .
  LAngle,         // <
  RAngle,         // >
  LParen,         // (
  RParen,         // )
  LBracket,       // [
  RBracket,       // ]
  Equals,         // ==

  // Special
  Eof,
  Error,
};

struct Token {
  TokenType type{TokenType::Error};
  std::string value{};
  std::uint32_t line{1};
  std::uint32_t column{1};

  [[nodiscard]] bool is(TokenType t) const noexcept { return type == t; }
  [[nodiscard]] bool is_item_type() const noexcept {
    return type >= TokenType::KwL && type <= TokenType::KwF8;
  }
};

[[nodiscard]] constexpr std::string_view token_type_name(TokenType t) noexcept {
  switch (t) {
    case TokenType::Identifier: return "Identifier";
    case TokenType::String: return "String";
    case TokenType::Integer: return "Integer";
    case TokenType::Float: return "Float";
    case TokenType::KwIf: return "if";
    case TokenType::KwEvery: return "every";
    case TokenType::KwSend: return "send";
    case TokenType::KwW: return "W";
    case TokenType::KwL: return "L";
    case TokenType::KwA: return "A";
    case TokenType::KwB: return "B";
    case TokenType::KwBoolean: return "Boolean";
    case TokenType::KwU1: return "U1";
    case TokenType::KwU2: return "U2";
    case TokenType::KwU4: return "U4";
    case TokenType::KwU8: return "U8";
    case TokenType::KwI1: return "I1";
    case TokenType::KwI2: return "I2";
    case TokenType::KwI4: return "I4";
    case TokenType::KwI8: return "I8";
    case TokenType::KwF4: return "F4";
    case TokenType::KwF8: return "F8";
    case TokenType::Colon: return ":";
    case TokenType::Dot: return ".";
    case TokenType::LAngle: return "<";
    case TokenType::RAngle: return ">";
    case TokenType::LParen: return "(";
    case TokenType::RParen: return ")";
    case TokenType::LBracket: return "[";
    case TokenType::RBracket: return "]";
    case TokenType::Equals: return "==";
    case TokenType::Eof: return "EOF";
    case TokenType::Error: return "Error";
  }
  return "Unknown";
}

}  // namespace secs::sml
