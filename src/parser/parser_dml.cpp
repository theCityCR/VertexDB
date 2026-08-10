#include "VertexDB/parser/parser.hpp"

#include "parse_utils.hpp"

#include <optional>
#include <stdexcept>

namespace VertexDB {

Insert Parser::parseInsert() {
    expect(TokenType::Identifier, "INTO");
    const auto table = advance();
    if (table.type != TokenType::Identifier) {
        throw std::runtime_error("expected table name");
    }
    expect(TokenType::Identifier, "VALUES");
    std::vector<std::vector<Value>> rows;
    do {
        expect(TokenType::LeftParen);
        std::vector<Value> values;
        do {
            values.push_back(parseValue());
        } while (match(TokenType::Comma));
        expect(TokenType::RightParen);
        rows.push_back(std::move(values));
    } while (match(TokenType::Comma));
    return {table.lexeme, std::move(rows)};
}

Analyze Parser::parseAnalyze() {
    Analyze command;
    if (match(TokenType::Identifier, "TABLE")) {
        const auto &table = advance();
        if (table.type != TokenType::Identifier) {
            throw std::runtime_error("expected table name after ANALYZE TABLE");
        }
        command.table = table.lexeme;
    }
    return command;
}

Update Parser::parseUpdate() {
    const auto table = advance();
    if (table.type != TokenType::Identifier) {
        throw std::runtime_error("expected table name");
    }
    expect(TokenType::Identifier, "SET");
    const auto column = advance();
    if (column.type != TokenType::Identifier) {
        throw std::runtime_error("expected update column");
    }
    expect(TokenType::Equal);
    auto value = parseValue();
    std::optional<Predicate> where;
    if (match(TokenType::Identifier, "WHERE")) {
        where = parsePredicate();
    }
    return {table.lexeme, column.lexeme, std::move(value), std::move(where)};
}

Delete Parser::parseDelete() {
    expect(TokenType::Identifier, "FROM");
    const auto table = advance();
    if (table.type != TokenType::Identifier) {
        throw std::runtime_error("expected table name");
    }
    std::optional<Predicate> where;
    if (match(TokenType::Identifier, "WHERE")) {
        where = parsePredicate();
    }
    return {table.lexeme, std::move(where)};
}

PrepareStatement Parser::parsePrepare() {
    const auto name = advance();
    if (name.type != TokenType::Identifier) {
        throw std::runtime_error("expected prepared statement name");
    }
    expect(TokenType::Identifier, "AS");
    const auto sql = advance();
    if (sql.type != TokenType::String) {
        throw std::runtime_error("expected prepared SQL string");
    }
    return {name.lexeme, sql.lexeme};
}

ExecutePrepared Parser::parseExecutePrepared() {
    const auto name = advance();
    if (name.type != TokenType::Identifier) {
        throw std::runtime_error("expected prepared statement name");
    }

    std::vector<Value> parameters;
    if (match(TokenType::Identifier, "VALUES")) {
        expect(TokenType::LeftParen);
        do {
            parameters.push_back(parseValue());
        } while (match(TokenType::Comma));
        expect(TokenType::RightParen);
    }
    return {name.lexeme, std::move(parameters)};
}

} // namespace VertexDB
