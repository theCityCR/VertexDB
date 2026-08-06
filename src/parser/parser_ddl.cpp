#include "VertexDB/parser/parser.hpp"

#include "VertexDB/common/string_utils.hpp"
#include "VertexDB/parser/tokenizer.hpp"
#include "parse_utils.hpp"

#include <stdexcept>

namespace VertexDB {
CreateDatabase Parser::parseCreateDatabase() {
    const auto name = advance();
    if (name.type != TokenType::Identifier) {
        throw std::runtime_error("expected database name");
    }
    return {name.lexeme};
}

CreateTable Parser::parseCreateTable() {
    const auto table = advance();
    if (table.type != TokenType::Identifier) {
        throw std::runtime_error("expected table name");
    }
    expect(TokenType::LeftParen);

    std::vector<Column> columns;
    do {
        const auto columnName = advance();
        if (columnName.type != TokenType::Identifier) {
            throw std::runtime_error("expected column name");
        }
        const auto typeName = advance();
        if (typeName.type != TokenType::Identifier) {
            throw std::runtime_error("expected column type");
        }
        auto type = columnTypeFromString(typeName.lexeme);
        if (!type) {
            throw std::runtime_error("unsupported column type");
        }
        bool nullable = false;
        if (match(TokenType::Identifier, "NOT")) {
            expect(TokenType::Identifier, "NULL");
        } else if (match(TokenType::Identifier, "NULL")) {
            nullable = true;
        }
        columns.push_back({columnName.lexeme, *type, nullable});
    } while (match(TokenType::Comma));

    expect(TokenType::RightParen);
    return {table.lexeme, std::move(columns)};
}

DropTable Parser::parseDropTable() {
    expect(TokenType::Identifier, "TABLE");
    const auto table = advance();
    if (table.type != TokenType::Identifier) {
        throw std::runtime_error("expected table name");
    }
    return {table.lexeme};
}

RenameTable Parser::parseRenameTable() {
    expect(TokenType::Identifier, "TABLE");
    const auto oldName = advance();
    if (oldName.type != TokenType::Identifier) {
        throw std::runtime_error("expected source table name");
    }
    expect(TokenType::Identifier, "TO");
    const auto newName = advance();
    if (newName.type != TokenType::Identifier) {
        throw std::runtime_error("expected destination table name");
    }
    return {oldName.lexeme, newName.lexeme};
}

CreateIndex Parser::parseCreateIndex() {
    const auto index = advance();
    if (index.type != TokenType::Identifier) {
        throw std::runtime_error("expected index name");
    }
    expect(TokenType::Identifier, "ON");
    const auto table = advance();
    if (table.type != TokenType::Identifier) {
        throw std::runtime_error("expected indexed table name");
    }
    expect(TokenType::LeftParen);
    const auto column = advance();
    if (column.type != TokenType::Identifier) {
        throw std::runtime_error("expected indexed column name");
    }
    expect(TokenType::RightParen);
    return {index.lexeme, table.lexeme, column.lexeme};
}

} // namespace VertexDB
