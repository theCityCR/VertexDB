#include "VertexDB/parser/tokenizer.hpp"

#include "VertexDB/parser/parse_error.hpp"

#include <cctype>

namespace VertexDB {
namespace {

bool isIdentifierStart(char ch) {
    return std::isalpha(static_cast<unsigned char>(ch)) != 0 || ch == '_';
}

bool isIdentifierPart(char ch) {
    return std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '_' || ch == '.';
}

struct SourceCursor {
    std::size_t pos{0};
    std::size_t line{1};
    std::size_t column{1};

    void advanceChar(char ch) {
        ++pos;
        if (ch == '\n') {
            ++line;
            column = 1;
        } else {
            ++column;
        }
    }
};

Token makeToken(TokenType type, std::string lexeme, const SourceCursor &start) {
    return Token{type, std::move(lexeme), start.pos, start.line, start.column};
}

} // namespace

std::vector<Token> Tokenizer::tokenize(std::string_view sql) const {
    std::vector<Token> tokens;
    SourceCursor cursor;

    while (cursor.pos < sql.size()) {
        const char ch = sql[cursor.pos];
        if (std::isspace(static_cast<unsigned char>(ch)) != 0) {
            cursor.advanceChar(ch);
            continue;
        }

        const SourceCursor start = cursor;
        if (isIdentifierStart(ch)) {
            cursor.advanceChar(ch);
            while (cursor.pos < sql.size() && isIdentifierPart(sql[cursor.pos])) {
                cursor.advanceChar(sql[cursor.pos]);
            }
            tokens.push_back(makeToken(TokenType::Identifier,
                                       std::string{sql.substr(start.pos, cursor.pos - start.pos)},
                                       start));
            continue;
        }

        if (std::isdigit(static_cast<unsigned char>(ch)) != 0) {
            cursor.advanceChar(ch);
            while (cursor.pos < sql.size() &&
                   (std::isdigit(static_cast<unsigned char>(sql[cursor.pos])) != 0 ||
                    sql[cursor.pos] == '.')) {
                cursor.advanceChar(sql[cursor.pos]);
            }
            tokens.push_back(makeToken(TokenType::Number,
                                       std::string{sql.substr(start.pos, cursor.pos - start.pos)},
                                       start));
            continue;
        }

        if (ch == '"') {
            cursor.advanceChar(ch);
            std::string value;
            while (cursor.pos < sql.size() && sql[cursor.pos] != '"') {
                if (sql[cursor.pos] == '\\') {
                    cursor.advanceChar(sql[cursor.pos]);
                    if (cursor.pos >= sql.size()) {
                        throw ParseError("unterminated string literal", start.line, start.column,
                                         start.pos);
                    }
                }
                value.push_back(sql[cursor.pos]);
                cursor.advanceChar(sql[cursor.pos]);
            }
            if (cursor.pos >= sql.size()) {
                throw ParseError("unterminated string literal", start.line, start.column, start.pos);
            }
            cursor.advanceChar(sql[cursor.pos]); // closing quote
            tokens.push_back(makeToken(TokenType::String, std::move(value), start));
            continue;
        }

        switch (ch) {
        case ',':
            tokens.push_back(makeToken(TokenType::Comma, ",", start));
            break;
        case ';':
            tokens.push_back(makeToken(TokenType::Semicolon, ";", start));
            break;
        case '(':
            tokens.push_back(makeToken(TokenType::LeftParen, "(", start));
            break;
        case ')':
            tokens.push_back(makeToken(TokenType::RightParen, ")", start));
            break;
        case '*':
            tokens.push_back(makeToken(TokenType::Star, "*", start));
            break;
        case '=':
            tokens.push_back(makeToken(TokenType::Equal, "=", start));
            break;
        case '>':
            tokens.push_back(makeToken(TokenType::Greater, ">", start));
            break;
        case '<':
            tokens.push_back(makeToken(TokenType::Less, "<", start));
            break;
        case '+':
            tokens.push_back(makeToken(TokenType::Plus, "+", start));
            break;
        case '-':
            tokens.push_back(makeToken(TokenType::Minus, "-", start));
            break;
        case '?':
            tokens.push_back(makeToken(TokenType::Parameter, "?", start));
            break;
        default:
            throw ParseError("unexpected character in SQL input", start.line, start.column,
                             start.pos);
        }
        cursor.advanceChar(ch);
    }

    tokens.push_back(makeToken(TokenType::End, "", cursor));
    return tokens;
}

} // namespace VertexDB
