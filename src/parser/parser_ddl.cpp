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
    bool sawPrimaryKey = false;
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
        bool unique = false;
        bool primaryKey = false;
        bool sawNullability = false;
        while (true) {
            if (match(TokenType::Identifier, "NOT")) {
                expect(TokenType::Identifier, "NULL");
                if (sawNullability) {
                    throw std::runtime_error("conflicting NULL / NOT NULL");
                }
                nullable = false;
                sawNullability = true;
                continue;
            }
            if (match(TokenType::Identifier, "NULL")) {
                if (sawNullability) {
                    throw std::runtime_error("conflicting NULL / NOT NULL");
                }
                nullable = true;
                sawNullability = true;
                continue;
            }
            if (match(TokenType::Identifier, "PRIMARY")) {
                expect(TokenType::Identifier, "KEY");
                if (primaryKey) {
                    throw std::runtime_error("duplicate PRIMARY KEY on column");
                }
                primaryKey = true;
                unique = true;
                continue;
            }
            if (match(TokenType::Identifier, "UNIQUE")) {
                unique = true;
                continue;
            }
            break;
        }
        if (primaryKey && nullable) {
            throw std::runtime_error("PRIMARY KEY column cannot be NULL");
        }
        if (primaryKey) {
            if (sawPrimaryKey) {
                throw std::runtime_error("multiple PRIMARY KEY columns are not supported");
            }
            sawPrimaryKey = true;
            nullable = false;
        }
        columns.push_back({columnName.lexeme, *type, nullable, unique, primaryKey});
    } while (match(TokenType::Comma));

    expect(TokenType::RightParen);
    return {table.lexeme, std::move(columns)};
}

DropDatabase Parser::parseDropDatabase() {
    const auto name = advance();
    if (name.type != TokenType::Identifier) {
        throw std::runtime_error("expected database name");
    }
    return {name.lexeme};
}

DropTable Parser::parseDropTable() {
    expect(TokenType::Identifier, "TABLE");
    const auto table = advance();
    if (table.type != TokenType::Identifier) {
        throw std::runtime_error("expected table name");
    }
    return {table.lexeme};
}

DropIndex Parser::parseDropIndex() {
    const auto index = advance();
    if (index.type != TokenType::Identifier) {
        throw std::runtime_error("expected index name");
    }
    expect(TokenType::Identifier, "ON");
    const auto table = advance();
    if (table.type != TokenType::Identifier) {
        throw std::runtime_error("expected indexed table name");
    }
    return {index.lexeme, table.lexeme};
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
    if (match(TokenType::LeftParen)) {
        auto expression = parseIndexExpression();
        expect(TokenType::RightParen);
        expect(TokenType::RightParen);
        return CreateIndex{index.lexeme, table.lexeme, expression.column, std::move(expression)};
    }
    const auto column = advance();
    if (column.type != TokenType::Identifier) {
        throw std::runtime_error("expected indexed column name");
    }
    expect(TokenType::RightParen);
    return {index.lexeme, table.lexeme, column.lexeme, std::nullopt};
}

} // namespace VertexDB
