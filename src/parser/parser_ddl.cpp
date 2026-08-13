#include "VertexDB/parser/parser.hpp"

#include "VertexDB/common/string_utils.hpp"
#include "VertexDB/parser/tokenizer.hpp"
#include "VertexDB/storage/check_eval.hpp"
#include "VertexDB/storage/foreign_key.hpp"
#include "parse_utils.hpp"

#include <stdexcept>
#include <type_traits>
#include <unordered_set>
#include <utility>

namespace VertexDB {
namespace {

void collectCheckColumns(const Predicate &predicate, std::unordered_set<std::string> &columns) {
    std::visit(
        [&](const auto &node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, AndPred> || std::is_same_v<T, OrPred>) {
                collectCheckColumns(*node.left, columns);
                collectCheckColumns(*node.right, columns);
            } else if constexpr (std::is_same_v<T, ComparisonPred>) {
                columns.insert(node.column);
                if (node.rhsColumn) {
                    columns.insert(*node.rhsColumn);
                }
            }
        },
        predicate);
}

void validateCheckColumns(const Predicate &predicate, const std::vector<Column> &schema) {
    std::unordered_set<std::string> known;
    known.reserve(schema.size());
    for (const auto &column : schema) {
        known.insert(column.name);
    }
    std::unordered_set<std::string> referenced;
    collectCheckColumns(predicate, referenced);
    for (const auto &column : referenced) {
        if (!known.contains(column)) {
            throw std::runtime_error("CHECK references unknown column " + column);
        }
    }
}

} // namespace

CreateDatabase Parser::parseCreateDatabase() {
    const auto name = advance();
    if (name.type != TokenType::Identifier) {
        throw std::runtime_error("expected database name");
    }
    return {name.lexeme};
}

Predicate Parser::parseCheckConstraintBody() {
    expect(TokenType::LeftParen);
    auto predicate = parsePredicate();
    expect(TokenType::RightParen);
    assertSimpleCheckConstraint(predicate);
    return predicate;
}

Predicate Parser::parseCheckConstraintExpression(std::string_view expression) {
    const auto tokens = Tokenizer{}.tokenize(expression);
    tokens_ = tokens;
    current_ = 0;
    nextParameterIndex_ = 0;
    currentFromTable_.clear();
    outerTableStack_.clear();
    auto predicate = parsePredicate();
    if (current_ < tokens_.size() && peek().type != TokenType::End) {
        throw std::runtime_error("unexpected tokens after CHECK expression");
    }
    assertSimpleCheckConstraint(predicate);
    return predicate;
}

CreateTable Parser::parseCreateTable() {
    const auto table = advance();
    if (table.type != TokenType::Identifier) {
        throw std::runtime_error("expected table name");
    }
    expect(TokenType::LeftParen);

    std::vector<Column> columns;
    std::vector<Predicate> checkConstraints;
    std::vector<ForeignKeyConstraint> foreignKeys;
    bool sawPrimaryKey = false;
    do {
        if (match(TokenType::Identifier, "CHECK")) {
            checkConstraints.push_back(parseCheckConstraintBody());
            continue;
        }
        if (match(TokenType::Identifier, "FOREIGN")) {
            expect(TokenType::Identifier, "KEY");
            expect(TokenType::LeftParen);
            const auto childColumn = advance();
            if (childColumn.type != TokenType::Identifier) {
                throw std::runtime_error("expected FOREIGN KEY column");
            }
            expect(TokenType::RightParen);
            expect(TokenType::Identifier, "REFERENCES");
            auto fk = parseReferencesClause(childColumn.lexeme);
            parseForeignKeyActions(fk);
            foreignKeys.push_back(std::move(fk));
            continue;
        }
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
            if (match(TokenType::Identifier, "CHECK")) {
                checkConstraints.push_back(parseCheckConstraintBody());
                continue;
            }
            if (match(TokenType::Identifier, "REFERENCES")) {
                auto fk = parseReferencesClause(columnName.lexeme);
                parseForeignKeyActions(fk);
                foreignKeys.push_back(std::move(fk));
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
    for (const auto &check : checkConstraints) {
        validateCheckColumns(check, columns);
    }
    for (const auto &fk : foreignKeys) {
        bool found = false;
        for (const auto &column : columns) {
            if (column.name == fk.childColumn) {
                found = true;
                break;
            }
        }
        if (!found) {
            throw std::runtime_error("FOREIGN KEY child column not found: " + fk.childColumn);
        }
    }
    return {table.lexeme, std::move(columns), std::move(checkConstraints), std::move(foreignKeys)};
}

ForeignKeyConstraint Parser::parseReferencesClause(std::string childColumn) {
    const auto parentTable = advance();
    if (parentTable.type != TokenType::Identifier) {
        throw std::runtime_error("expected REFERENCES table name");
    }
    expect(TokenType::LeftParen);
    const auto parentColumn = advance();
    if (parentColumn.type != TokenType::Identifier) {
        throw std::runtime_error("expected REFERENCES column name");
    }
    expect(TokenType::RightParen);
    return ForeignKeyConstraint{std::move(childColumn), parentTable.lexeme, parentColumn.lexeme};
}

void Parser::parseForeignKeyActions(ForeignKeyConstraint &fk) {
    while (match(TokenType::Identifier, "ON")) {
        const bool isDelete = match(TokenType::Identifier, "DELETE");
        if (!isDelete) {
            expect(TokenType::Identifier, "UPDATE");
        }
        expect(TokenType::Identifier, "NO");
        expect(TokenType::Identifier, "ACTION");
        if (isDelete) {
            fk.onDelete = ForeignKeyAction::NoAction;
        } else {
            fk.onUpdate = ForeignKeyAction::NoAction;
        }
    }
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
