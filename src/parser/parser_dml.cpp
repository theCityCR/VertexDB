#include "VertexDB/parser/parser.hpp"

#include "parse_utils.hpp"

#include <memory>
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

Select Parser::parseSelect() {
    expect(TokenType::Identifier, "SELECT");
    return parseSelectAfterSelectKeyword();
}

Select Parser::parseSelectAfterSelectKeyword() {
    std::vector<std::string> columns;
    if (match(TokenType::Star)) {
        columns.emplace_back("*");
    } else {
        do {
            const auto column = advance();
            if (column.type != TokenType::Identifier) {
                throw std::runtime_error("expected selected column");
            }
            columns.push_back(column.lexeme);
        } while (match(TokenType::Comma));
    }

    expect(TokenType::Identifier, "FROM");

    std::string tableName;
    std::vector<std::pair<std::string, std::shared_ptr<Select>>> derivedCtes;
    if (match(TokenType::LeftParen)) {
        // Derived table: FROM (SELECT ...) [AS] alias — normalize to a synthetic CTE.
        expect(TokenType::Identifier, "SELECT");
        auto body = parseSelectAfterSelectKeyword();
        expect(TokenType::RightParen);
        (void)match(TokenType::Identifier, "AS");
        const auto alias = advance();
        if (alias.type != TokenType::Identifier) {
            throw std::runtime_error("expected derived table alias");
        }
        tableName = alias.lexeme;
        derivedCtes.emplace_back(alias.lexeme, std::make_shared<Select>(std::move(body)));
    } else {
        const auto table = advance();
        if (table.type != TokenType::Identifier) {
            throw std::runtime_error("expected table name");
        }
        tableName = table.lexeme;
    }

    std::optional<JoinClause> join;
    if (match(TokenType::Identifier, "JOIN")) {
        const auto joinedTable = advance();
        if (joinedTable.type != TokenType::Identifier) {
            throw std::runtime_error("expected joined table name");
        }
        expect(TokenType::Identifier, "ON");
        const auto leftColumn = advance();
        if (leftColumn.type != TokenType::Identifier) {
            throw std::runtime_error("expected left join column");
        }
        expect(TokenType::Equal);
        const auto rightColumn = advance();
        if (rightColumn.type != TokenType::Identifier) {
            throw std::runtime_error("expected right join column");
        }
        join = JoinClause{joinedTable.lexeme, leftColumn.lexeme, rightColumn.lexeme};
    }

    std::optional<Predicate> where;
    std::optional<OrderBy> orderBy;
    std::optional<std::size_t> limit;
    if (match(TokenType::Identifier, "WHERE")) {
        where = parsePredicate();
    }
    if (match(TokenType::Identifier, "ORDER")) {
        expect(TokenType::Identifier, "BY");
        const auto column = advance();
        if (column.type != TokenType::Identifier) {
            throw std::runtime_error("expected ORDER BY column");
        }
        bool ascending = true;
        if (match(TokenType::Identifier, "DESC")) {
            ascending = false;
        } else {
            (void)match(TokenType::Identifier, "ASC");
        }
        orderBy = OrderBy{column.lexeme, ascending};
    }
    if (match(TokenType::Identifier, "LIMIT")) {
        const auto count = advance();
        if (count.type != TokenType::Number) {
            throw std::runtime_error("expected numeric limit");
        }
        limit = static_cast<std::size_t>(parser_detail::parseIntLiteral(count.lexeme));
    }
    return {std::move(tableName), std::move(join),    std::move(columns),
            std::move(where),      std::move(orderBy), limit,
            std::move(derivedCtes)};
}

Select Parser::parseWithSelect() {
    std::vector<std::pair<std::string, std::shared_ptr<Select>>> ctes;
    do {
        const auto name = advance();
        if (name.type != TokenType::Identifier) {
            throw std::runtime_error("expected CTE name");
        }
        expect(TokenType::Identifier, "AS");
        expect(TokenType::LeftParen);
        if (match(TokenType::Identifier, "WITH")) {
            throw std::runtime_error("nested WITH inside CTE is not supported");
        }
        auto body = parseSelect();
        expect(TokenType::RightParen);
        ctes.emplace_back(name.lexeme, std::make_shared<Select>(std::move(body)));
    } while (match(TokenType::Comma));

    expect(TokenType::Identifier, "SELECT");
    auto query = parseSelectAfterSelectKeyword();
    // Derived-table CTEs are closest to FROM; append WITH CTEs after so a colliding FROM alias
    // prefers the derived table, while WITH names remain available for bodies/siblings.
    if (!query.ctes.empty()) {
        auto combined = std::move(query.ctes);
        for (auto &cte : ctes) {
            combined.push_back(std::move(cte));
        }
        query.ctes = std::move(combined);
    } else {
        query.ctes = std::move(ctes);
    }
    return query;
}

ExplainQuery Parser::parseExplain() {
    if (match(TokenType::Identifier, "WITH")) {
        return ExplainQuery{parseWithSelect()};
    }
    expect(TokenType::Identifier, "SELECT");
    return ExplainQuery{parseSelectAfterSelectKeyword()};
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
