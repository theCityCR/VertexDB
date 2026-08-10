#include "VertexDB/parser/parser.hpp"

#include "VertexDB/common/string_utils.hpp"
#include "parse_utils.hpp"

#include <memory>
#include <optional>
#include <stdexcept>

namespace VertexDB {
namespace {

[[nodiscard]] std::optional<AggregateOp> aggregateOpFromName(std::string_view name) {
    if (equalsIgnoreCase(name, "COUNT")) {
        return AggregateOp::Count;
    }
    if (equalsIgnoreCase(name, "SUM")) {
        return AggregateOp::Sum;
    }
    if (equalsIgnoreCase(name, "AVG")) {
        return AggregateOp::Avg;
    }
    if (equalsIgnoreCase(name, "MIN")) {
        return AggregateOp::Min;
    }
    if (equalsIgnoreCase(name, "MAX")) {
        return AggregateOp::Max;
    }
    return std::nullopt;
}

} // namespace

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
    std::vector<SelectExpr> columns;
    if (match(TokenType::Star)) {
        columns.push_back(SelectExpr::makeStar());
    } else {
        do {
            if (peek().type == TokenType::Identifier) {
                const auto agg = aggregateOpFromName(peek().lexeme);
                if (agg && current_ + 1 < tokens_.size() &&
                    tokens_[current_ + 1].type == TokenType::LeftParen) {
                    advance(); // aggregate name
                    expect(TokenType::LeftParen);
                    if (*agg == AggregateOp::Count && match(TokenType::Star)) {
                        expect(TokenType::RightParen);
                        columns.push_back(SelectExpr::makeAggregate(AggregateOp::CountStar));
                    } else {
                        const auto arg = advance();
                        if (arg.type != TokenType::Identifier) {
                            throw std::runtime_error("expected aggregate column argument");
                        }
                        expect(TokenType::RightParen);
                        if (*agg == AggregateOp::Count) {
                            columns.push_back(
                                SelectExpr::makeAggregate(AggregateOp::Count, arg.lexeme));
                        } else {
                            columns.push_back(SelectExpr::makeAggregate(*agg, arg.lexeme));
                        }
                    }
                    continue;
                }
            }
            const auto column = advance();
            if (column.type != TokenType::Identifier) {
                throw std::runtime_error("expected selected column");
            }
            columns.push_back(SelectExpr::makeColumn(column.lexeme));
        } while (match(TokenType::Comma));
    }

    expect(TokenType::Identifier, "FROM");

    std::string tableName;
    std::optional<std::string> tableAlias;
    std::vector<CteEntry> derivedCtes;
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
        derivedCtes.push_back(
            CteEntry{alias.lexeme, std::make_shared<Select>(std::move(body)),
                     MaterializeMode::DefaultInline});
    } else {
        const auto table = advance();
        if (table.type != TokenType::Identifier) {
            throw std::runtime_error("expected table name");
        }
        tableName = table.lexeme;
        (void)match(TokenType::Identifier, "AS");
        if (peek().type == TokenType::Identifier &&
            !parser_detail::isSelectClauseKeyword(peek().lexeme)) {
            tableAlias = advance().lexeme;
        }
    }

    std::vector<JoinClause> joins;
    while (peek().type == TokenType::Identifier &&
           parser_detail::isJoinIntroducer(peek().lexeme)) {
        JoinKind joinKind = JoinKind::Inner;
        if (match(TokenType::Identifier, "LEFT")) {
            (void)match(TokenType::Identifier, "OUTER");
            expect(TokenType::Identifier, "JOIN");
            joinKind = JoinKind::LeftOuter;
        } else if (match(TokenType::Identifier, "INNER")) {
            expect(TokenType::Identifier, "JOIN");
        } else if (match(TokenType::Identifier, "RIGHT") || match(TokenType::Identifier, "FULL") ||
                   match(TokenType::Identifier, "CROSS")) {
            throw std::runtime_error("only INNER and LEFT [OUTER] JOIN are supported");
        } else {
            expect(TokenType::Identifier, "JOIN");
        }

        const auto joinedTable = advance();
        if (joinedTable.type != TokenType::Identifier) {
            throw std::runtime_error("expected joined table name");
        }
        std::optional<std::string> joinedAlias;
        (void)match(TokenType::Identifier, "AS");
        if (peek().type == TokenType::Identifier &&
            !parser_detail::isSelectClauseKeyword(peek().lexeme)) {
            joinedAlias = advance().lexeme;
        }
        expect(TokenType::Identifier, "ON");
        const auto leftColumn = advance();
        if (leftColumn.type != TokenType::Identifier) {
            throw std::runtime_error("expected left join column");
        }
        ComparisonOperator joinOp = ComparisonOperator::Equal;
        if (match(TokenType::Equal)) {
            joinOp = ComparisonOperator::Equal;
        } else if (match(TokenType::Greater)) {
            joinOp = ComparisonOperator::Greater;
        } else if (match(TokenType::Less)) {
            joinOp = ComparisonOperator::Less;
        } else {
            throw std::runtime_error("expected join comparison operator");
        }
        const auto rightColumn = advance();
        if (rightColumn.type != TokenType::Identifier) {
            throw std::runtime_error("expected right join column");
        }
        joins.push_back(JoinClause{joinedTable.lexeme, leftColumn.lexeme, rightColumn.lexeme,
                                   std::move(joinedAlias), joinKind, joinOp});
    }

    std::optional<Predicate> where;
    std::vector<std::string> groupBy;
    std::optional<OrderBy> orderBy;
    std::optional<std::size_t> limit;
    const auto previousFromTable = currentFromTable_;
    currentFromTable_ = tableAlias.value_or(tableName);
    if (match(TokenType::Identifier, "WHERE")) {
        where = parsePredicate();
    }
    currentFromTable_ = previousFromTable;
    if (match(TokenType::Identifier, "GROUP")) {
        expect(TokenType::Identifier, "BY");
        do {
            const auto column = advance();
            if (column.type != TokenType::Identifier) {
                throw std::runtime_error("expected GROUP BY column");
            }
            groupBy.push_back(column.lexeme);
        } while (match(TokenType::Comma));
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
    Select query{std::move(tableName), std::move(joins),   std::move(columns),
                 std::move(where),     std::move(groupBy), std::move(orderBy),
                 limit,                std::move(derivedCtes)};
    query.tableAlias = std::move(tableAlias);
    return query;
}

Select Parser::parseWithSelect() { return parseWithSelectAtDepth(0); }

Select Parser::parseWithSelectAtDepth(int depth) {
    std::vector<CteEntry> ctes;
    do {
        const auto name = advance();
        if (name.type != TokenType::Identifier) {
            throw std::runtime_error("expected CTE name");
        }
        expect(TokenType::Identifier, "AS");
        MaterializeMode mode = MaterializeMode::DefaultInline;
        if (match(TokenType::Identifier, "NOT")) {
            expect(TokenType::Identifier, "MATERIALIZED");
            mode = MaterializeMode::NotMaterialized;
        } else if (match(TokenType::Identifier, "MATERIALIZED")) {
            mode = MaterializeMode::Materialized;
        }
        expect(TokenType::LeftParen);
        Select body;
        if (match(TokenType::Identifier, "WITH")) {
            if (depth >= kMaxNestedWithDepth) {
                throw std::runtime_error("nested WITH exceeds maximum depth");
            }
            body = parseWithSelectAtDepth(depth + 1);
        } else {
            body = parseSelect();
        }
        expect(TokenType::RightParen);
        ctes.push_back(CteEntry{name.lexeme, std::make_shared<Select>(std::move(body)), mode});
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
