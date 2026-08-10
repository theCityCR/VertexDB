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
        } else if (match(TokenType::Identifier, "RIGHT")) {
            (void)match(TokenType::Identifier, "OUTER");
            expect(TokenType::Identifier, "JOIN");
            joinKind = JoinKind::RightOuter;
        } else if (match(TokenType::Identifier, "FULL")) {
            (void)match(TokenType::Identifier, "OUTER");
            expect(TokenType::Identifier, "JOIN");
            joinKind = JoinKind::FullOuter;
        } else if (match(TokenType::Identifier, "CROSS")) {
            expect(TokenType::Identifier, "JOIN");
            joinKind = JoinKind::Cross;
        } else if (match(TokenType::Identifier, "INNER")) {
            expect(TokenType::Identifier, "JOIN");
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
        if (joinKind == JoinKind::Cross) {
            joins.push_back(JoinClause{joinedTable.lexeme, {}, {}, std::move(joinedAlias),
                                       joinKind, ComparisonOperator::Equal});
            continue;
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
    const bool recursiveWith = match(TokenType::Identifier, "RECURSIVE");
    std::vector<CteEntry> ctes;
    std::size_t recursiveCount = 0;
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
        std::shared_ptr<Select> recursiveArm;
        bool recursive = false;
        if (match(TokenType::Identifier, "UNION")) {
            if (!match(TokenType::Identifier, "ALL")) {
                throw std::runtime_error("WITH RECURSIVE requires UNION ALL");
            }
            if (!recursiveWith) {
                throw std::runtime_error("UNION ALL in CTE requires WITH RECURSIVE");
            }
            Select arm;
            if (match(TokenType::Identifier, "WITH")) {
                throw std::runtime_error("WITH inside recursive arm is not supported");
            }
            arm = parseSelect();
            recursiveArm = std::make_shared<Select>(std::move(arm));
            recursive = true;
            ++recursiveCount;
            if (recursiveCount > 1) {
                throw std::runtime_error("only one recursive CTE is supported");
            }
            auto countSelfRefs = [](const Select &select, std::string_view cteName) {
                std::size_t count = 0;
                if (equalsIgnoreCase(select.table, cteName)) {
                    ++count;
                }
                for (const auto &join : select.joins) {
                    if (equalsIgnoreCase(join.table, cteName)) {
                        ++count;
                    }
                }
                return count;
            };
            if (countSelfRefs(body, name.lexeme) != 0) {
                throw std::runtime_error("recursive CTE anchor must not reference itself");
            }
            if (countSelfRefs(*recursiveArm, name.lexeme) != 1) {
                throw std::runtime_error(
                    "recursive CTE arm must reference the CTE name exactly once");
            }
        }
        expect(TokenType::RightParen);
        ctes.push_back(CteEntry{name.lexeme, std::make_shared<Select>(std::move(body)), mode,
                                recursive, std::move(recursiveArm)});
    } while (match(TokenType::Comma));

    if (recursiveWith && recursiveCount == 0) {
        throw std::runtime_error("WITH RECURSIVE requires a UNION ALL recursive CTE");
    }

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

Select Parser::parseSubquerySelect(bool /*allowOuterRefs*/) {
    if (currentFromTable_.empty()) {
        throw std::runtime_error("subquery is missing an outer FROM scope");
    }
    outerTableStack_.push_back(currentFromTable_);
    const auto savedFrom = currentFromTable_;
    Select subquery;
    if (match(TokenType::Identifier, "WITH")) {
        subquery = parseWithSelectAtDepth(0);
    } else {
        expect(TokenType::Identifier, "SELECT");
        subquery = parseSelectAfterSelectKeyword();
    }
    currentFromTable_ = savedFrom;
    const bool nestedUnderCorrelated = outerTableStack_.size() > 1;
    markOuterRefs(subquery, selectScopeName(subquery), nestedUnderCorrelated);
    outerTableStack_.pop_back();
    return subquery;
}

void Parser::markOuterRefs(Select &select, std::string_view innerTable,
                           bool nestedUnderCorrelated) {
    for (auto &cte : select.ctes) {
        if (!cte.body) {
            continue;
        }
        markOuterRefs(*cte.body, selectScopeName(*cte.body), nestedUnderCorrelated);
        if (cte.body->hasOuterRefs) {
            select.hasOuterRefs = true;
        }
    }
    if (select.where) {
        markOuterRefs(*select.where, innerTable, nestedUnderCorrelated);
        if (predicateReferencesOuter(*select.where)) {
            select.hasOuterRefs = true;
        }
    }
}

void Parser::markOuterRefs(Predicate &predicate, std::string_view innerTable,
                           bool nestedUnderCorrelated) {
    using parser_detail::columnQualifier;
    using parser_detail::refersToOuterTable;
    (void)nestedUnderCorrelated;
    std::visit(
        [&](auto &node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, AndPred> || std::is_same_v<T, OrPred>) {
                markOuterRefs(*node.left, innerTable, nestedUnderCorrelated);
                markOuterRefs(*node.right, innerTable, nestedUnderCorrelated);
            } else if constexpr (std::is_same_v<T, InSubqueryPred> ||
                                 std::is_same_v<T, ExistsPred>) {
                if (node.subquery && node.subquery->hasOuterRefs) {
                    node.referencesOuter = true;
                    if (outerTableStack_.size() > kMaxOuterCorrelationDepth) {
                        throw std::runtime_error(
                            "correlated subquery exceeds maximum outer depth");
                    }
                }
            } else if constexpr (std::is_same_v<T, ComparisonPred>) {
                bool outer = refersToOuterTable(node.column, innerTable, outerTableStack_);
                if (node.rhsColumn) {
                    if (refersToOuterTable(*node.rhsColumn, innerTable, outerTableStack_) ||
                        (!columnQualifier(*node.rhsColumn) && !outerTableStack_.empty())) {
                        outer = true;
                    }
                }
                if (outer) {
                    // Allow up to four outer FROM frames while correlating.
                    if (outerTableStack_.size() > kMaxOuterCorrelationDepth) {
                        throw std::runtime_error(
                            "correlated subquery exceeds maximum outer depth");
                    }
                    node.referencesOuter = true;
                }
            } else if constexpr (std::is_same_v<T, LikePred> || std::is_same_v<T, RegexPred> ||
                                 std::is_same_v<T, InListPred>) {
                if (refersToOuterTable(node.column, innerTable, outerTableStack_)) {
                    if (outerTableStack_.size() > kMaxOuterCorrelationDepth) {
                        throw std::runtime_error(
                            "correlated subquery exceeds maximum outer depth");
                    }
                    node.referencesOuter = true;
                }
            }
        },
        predicate);
}

} // namespace VertexDB
