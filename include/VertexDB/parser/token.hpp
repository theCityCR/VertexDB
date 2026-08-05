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
    End,
};

struct Token {
    TokenType type;
    std::string lexeme;
};

} // namespace VertexDB
