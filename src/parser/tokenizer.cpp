#include "VertexDB/parser/tokenizer.hpp"

#include <cctype>
#include <stdexcept>

namespace VertexDB {
namespace {

bool isIdentifierStart(char ch) {
    return std::isalpha(static_cast<unsigned char>(ch)) != 0 || ch == '_';
}

bool isIdentifierPart(char ch) {
    return std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '_' || ch == '.';
}

} // namespace

std::vector<Token> Tokenizer::tokenize(std::string_view sql) const {
    std::vector<Token> tokens;
    std::size_t pos = 0;

    while (pos < sql.size()) {
        const char ch = sql[pos];
        if (std::isspace(static_cast<unsigned char>(ch)) != 0) {
            ++pos;
            continue;
        }

        if (isIdentifierStart(ch)) {
            const std::size_t start = pos++;
            while (pos < sql.size() && isIdentifierPart(sql[pos])) {
                ++pos;
            }
            tokens.push_back({TokenType::Identifier, std::string{sql.substr(start, pos - start)}});
            continue;
        }

        if (std::isdigit(static_cast<unsigned char>(ch)) != 0 || ch == '-') {
            const std::size_t start = pos++;
            while (pos < sql.size() &&
                   (std::isdigit(static_cast<unsigned char>(sql[pos])) != 0 || sql[pos] == '.')) {
                ++pos;
            }
            tokens.push_back({TokenType::Number, std::string{sql.substr(start, pos - start)}});
            continue;
        }

        if (ch == '"') {
            ++pos;
            std::string value;
            while (pos < sql.size() && sql[pos] != '"') {
                if (sql[pos] == '\\') {
                    ++pos;
                    if (pos >= sql.size()) {
                        throw std::runtime_error("unterminated string literal");
                    }
                }
                value.push_back(sql[pos]);
                ++pos;
            }
            if (pos >= sql.size()) {
                throw std::runtime_error("unterminated string literal");
            }
            tokens.push_back({TokenType::String, std::move(value)});
            ++pos;
            continue;
        }

        switch (ch) {
        case ',':
            tokens.push_back({TokenType::Comma, ","});
            break;
        case ';':
            tokens.push_back({TokenType::Semicolon, ";"});
            break;
        case '(':
            tokens.push_back({TokenType::LeftParen, "("});
            break;
        case ')':
            tokens.push_back({TokenType::RightParen, ")"});
            break;
        case '*':
            tokens.push_back({TokenType::Star, "*"});
            break;
        case '=':
            tokens.push_back({TokenType::Equal, "="});
            break;
        case '>':
            tokens.push_back({TokenType::Greater, ">"});
            break;
        case '<':
            tokens.push_back({TokenType::Less, "<"});
            break;
        default:
            throw std::runtime_error("unexpected character in SQL input");
        }
        ++pos;
    }

    tokens.push_back({TokenType::End, ""});
    return tokens;
}

} // namespace VertexDB
