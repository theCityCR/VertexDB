#pragma once

#include <cstdint>
#include <string>

namespace VertexDB {

enum class TokenType : std::uint8_t {
    Identifier,
    Number,
    String,
    Comma,
    Semicolon,
    LeftParen,
    RightParen,
    Star,
    Equal,
    Greater,
    Less,
    Plus,
    Minus,
    Parameter,
    Tilde,
    End,
};

struct Token {
    TokenType type;
    std::string lexeme;
    // 0-based byte offset into the original SQL; line/column are 1-based.
    std::size_t offset{0};
    std::size_t line{1};
    std::size_t column{1};
};

} // namespace VertexDB
