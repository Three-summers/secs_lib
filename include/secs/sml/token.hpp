#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace secs::sml {

enum class TokenType : std::uint8_t {
    // 字面量
    Identifier, // 名称、S1F1 等
    String,     // "..."/'...'
    Integer,    // 123、0x1F
    Float,      // 0.5567

    // 关键字
    KwIf,    // 关键字 "if"
    KwEvery, // 关键字 "every"
    KwSend,  // 关键字 "send"
    KwW,     // W（等待位）

    // 数据项类型（SECS-II 数据项）
    KwL,       // L（列表）
    KwA,       // A（ASCII 字符串）
    KwB,       // B（二进制）
    KwBoolean, // Boolean（布尔）
    KwU1,
    KwU2,
    KwU4,
    KwU8, // 无符号整数
    KwI1,
    KwI2,
    KwI4,
    KwI8, // 有符号整数
    KwF4,
    KwF8, // 浮点数

    // 标点
    Colon,    // :
    Dot,      // .
    LAngle,   // <
    RAngle,   // >
    LParen,   // (
    RParen,   // )
    LBracket, // [
    RBracket, // ]
    Equals,   // ==

    // 特殊
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
    case TokenType::Identifier:
        return "Identifier";
    case TokenType::String:
        return "String";
    case TokenType::Integer:
        return "Integer";
    case TokenType::Float:
        return "Float";
    case TokenType::KwIf:
        return "if";
    case TokenType::KwEvery:
        return "every";
    case TokenType::KwSend:
        return "send";
    case TokenType::KwW:
        return "W";
    case TokenType::KwL:
        return "L";
    case TokenType::KwA:
        return "A";
    case TokenType::KwB:
        return "B";
    case TokenType::KwBoolean:
        return "Boolean";
    case TokenType::KwU1:
        return "U1";
    case TokenType::KwU2:
        return "U2";
    case TokenType::KwU4:
        return "U4";
    case TokenType::KwU8:
        return "U8";
    case TokenType::KwI1:
        return "I1";
    case TokenType::KwI2:
        return "I2";
    case TokenType::KwI4:
        return "I4";
    case TokenType::KwI8:
        return "I8";
    case TokenType::KwF4:
        return "F4";
    case TokenType::KwF8:
        return "F8";
    case TokenType::Colon:
        return ":";
    case TokenType::Dot:
        return ".";
    case TokenType::LAngle:
        return "<";
    case TokenType::RAngle:
        return ">";
    case TokenType::LParen:
        return "(";
    case TokenType::RParen:
        return ")";
    case TokenType::LBracket:
        return "[";
    case TokenType::RBracket:
        return "]";
    case TokenType::Equals:
        return "==";
    case TokenType::Eof:
        return "EOF";
    case TokenType::Error:
        return "Error";
    }
    return "Unknown";
}

} // namespace secs::sml
