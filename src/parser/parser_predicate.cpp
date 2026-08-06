#include "VertexDB/parser/parser.hpp"

#include "VertexDB/common/string_utils.hpp"
#include "parse_utils.hpp"

#include <memory>
#include <stdexcept>

namespace VertexDB {
namespace {

[[nodiscard]] std::optional<std::string_view> qualifier(std::string_view name) {
    const auto dot = name.find('.');
    if (dot == std::string_view::npos) {
        return std::nullopt;
    }
    return name.substr(0, dot);
}

[[nodiscard]] bool refersToOuterTable(std::string_view column, std::string_view innerTable,
                                      const std::vector<std::string> &outerTables) {
    if (const auto table = qualifier(column)) {
        if (equalsIgnoreCase(*table, innerTable)) {
            return false;
        }
        for (const auto &outer : outerTables) {
            if (equalsIgnoreCase(*table, outer)) {
                return true;
            }
        }
        // Qualified name that is not the inner table is treated as an outer reference.
        return !outerTables.empty();
    }
    return false;
}

} // namespace

Predicate Parser::parsePredicate() { return parseOrPredicate(); }

Predicate Parser::parseOrPredicate() {
    auto predicate = parseAndPredicate();
    while (match(TokenType::Identifier, "OR")) {
        auto left = std::make_shared<Predicate>(std::move(predicate));
        auto right = std::make_shared<Predicate>(parseAndPredicate());
        predicate = Predicate{Predicate::Kind::Or, std::move(left), std::move(right)};
    }
    return predicate;
}

Predicate Parser::parseAndPredicate() {
    auto predicate = parsePrimaryPredicate();
    while (match(TokenType::Identifier, "AND")) {
        auto left = std::make_shared<Predicate>(std::move(predicate));
        auto right = std::make_shared<Predicate>(parsePrimaryPredicate());
        predicate = Predicate{Predicate::Kind::And, std::move(left), std::move(right)};
    }
    return predicate;
}

Predicate Parser::parsePrimaryPredicate() {
    if (match(TokenType::Identifier, "EXISTS")) {
        return parseExistsPredicate();
    }
    if (match(TokenType::LeftParen)) {
        // Expression comparison: (expr) op value — used to hit expression indexes.
        if (peek().type == TokenType::Identifier || peek().type == TokenType::Minus) {
            const auto saved = current_;
            try {
                auto expression = parseIndexExpression();
                expect(TokenType::RightParen);
                ComparisonOperator op{};
                if (match(TokenType::Equal)) {
                    op = ComparisonOperator::Equal;
                } else if (match(TokenType::Greater)) {
                    op = ComparisonOperator::Greater;
                } else if (match(TokenType::Less)) {
                    op = ComparisonOperator::Less;
                } else {
                    throw std::runtime_error("expected comparison operator");
                }
                return Predicate::makeExpressionComparison(std::move(expression), op, parseValue());
            } catch (const std::runtime_error &) {
                current_ = saved;
            }
        }
        auto predicate = parsePredicate();
        expect(TokenType::RightParen);
        return predicate;
    }
    return parseComparisonPredicate();
}

Predicate Parser::parseExistsPredicate() {
    expect(TokenType::LeftParen);
    expect(TokenType::Identifier, "SELECT");
    auto subquery = std::make_shared<Select>(parseSubquerySelect(true));
    expect(TokenType::RightParen);
    auto predicate = Predicate::makeExists(std::move(subquery));
    if (predicate.subquery && predicate.subquery->hasOuterRefs) {
        predicate.referencesOuter = true;
    }
    return predicate;
}

IndexExpression Parser::parseIndexExpression() {
    if (match(TokenType::Minus)) {
        const auto column = advance();
        if (column.type != TokenType::Identifier) {
            throw std::runtime_error("expected column after unary minus");
        }
        if (qualifier(column.lexeme)) {
            throw std::runtime_error("expression index column must be unqualified");
        }
        return IndexExpression{IndexExpression::Kind::Negate, column.lexeme, {}};
    }

    const auto column = advance();
    if (column.type != TokenType::Identifier) {
        throw std::runtime_error("expected expression column");
    }
    if (qualifier(column.lexeme)) {
        throw std::runtime_error("expression index column must be unqualified");
    }

    if (match(TokenType::Plus)) {
        return IndexExpression{IndexExpression::Kind::Add, column.lexeme, parseValue()};
    }
    if (match(TokenType::Minus)) {
        return IndexExpression{IndexExpression::Kind::Subtract, column.lexeme, parseValue()};
    }
    return IndexExpression{IndexExpression::Kind::Column, column.lexeme, {}};
}

Predicate Parser::parseComparisonPredicate() {
    const auto column = advance();
    if (column.type != TokenType::Identifier) {
        throw std::runtime_error("expected predicate column");
    }
    if (match(TokenType::Identifier, "IN")) {
        expect(TokenType::LeftParen);
        expect(TokenType::Identifier, "SELECT");
        auto subquery = std::make_shared<Select>(parseSubquerySelect(true));
        if (subquery->columns.size() != 1 || subquery->columns.front() == "*") {
            throw std::runtime_error("IN subquery must project exactly one column");
        }
        expect(TokenType::RightParen);
        Predicate predicate{column.lexeme, std::move(subquery)};
        if (predicate.subquery && predicate.subquery->hasOuterRefs) {
            predicate.referencesOuter = true;
        }
        return predicate;
    }

    ComparisonOperator op{};
    if (match(TokenType::Equal)) {
        op = ComparisonOperator::Equal;
    } else if (match(TokenType::Greater)) {
        op = ComparisonOperator::Greater;
    } else if (match(TokenType::Less)) {
        op = ComparisonOperator::Less;
    } else {
        throw std::runtime_error("expected comparison operator");
    }

    // RHS may be a literal or a column reference (including outer table.column).
    if (peek().type == TokenType::Identifier && !equalsIgnoreCase(peek().lexeme, "NULL")) {
        const auto rhs = advance();
        Predicate predicate;
        predicate.kind = Predicate::Kind::Comparison;
        predicate.column = column.lexeme;
        predicate.op = op;
        predicate.rhsColumn = rhs.lexeme;
        return predicate;
    }
    return {column.lexeme, op, parseValue()};
}

Select Parser::parseSubquerySelect(bool /*allowOuterRefs*/) {
    if (currentFromTable_.empty()) {
        throw std::runtime_error("subquery is missing an outer FROM scope");
    }
    outerTableStack_.push_back(currentFromTable_);
    const auto savedFrom = currentFromTable_;
    auto subquery = parseSelectAfterSelectKeyword();
    currentFromTable_ = savedFrom;
    if (!subquery.ctes.empty()) {
        outerTableStack_.pop_back();
        throw std::runtime_error("WITH inside subquery is not supported");
    }
    if (subquery.join) {
        outerTableStack_.pop_back();
        throw std::runtime_error("JOIN inside subquery is not supported");
    }
    const bool nestedUnderCorrelated = outerTableStack_.size() > 1;
    markOuterRefs(subquery, subquery.table, nestedUnderCorrelated);
    outerTableStack_.pop_back();
    return subquery;
}

void Parser::markOuterRefs(Select &select, std::string_view innerTable,
                           bool nestedUnderCorrelated) {
    if (select.where) {
        markOuterRefs(*select.where, innerTable, nestedUnderCorrelated);
        if (select.where->referencesOuter) {
            select.hasOuterRefs = true;
        }
    }
}

void Parser::markOuterRefs(Predicate &predicate, std::string_view innerTable,
                           bool nestedUnderCorrelated) {
    if (predicate.kind == Predicate::Kind::And || predicate.kind == Predicate::Kind::Or) {
        markOuterRefs(*predicate.left, innerTable, nestedUnderCorrelated);
        markOuterRefs(*predicate.right, innerTable, nestedUnderCorrelated);
        predicate.referencesOuter =
            predicate.left->referencesOuter || predicate.right->referencesOuter;
        return;
    }

    if (predicate.kind == Predicate::Kind::InSubquery || predicate.kind == Predicate::Kind::Exists) {
        if (predicate.subquery && predicate.subquery->hasOuterRefs) {
            predicate.referencesOuter = true;
            if (nestedUnderCorrelated) {
                throw std::runtime_error("multi-level correlated subqueries are not supported");
            }
        }
        return;
    }

    if (predicate.kind != Predicate::Kind::Comparison) {
        return;
    }

    bool outer = false;
    if (refersToOuterTable(predicate.column, innerTable, outerTableStack_)) {
        outer = true;
    }
    if (predicate.rhsColumn) {
        if (refersToOuterTable(*predicate.rhsColumn, innerTable, outerTableStack_)) {
            outer = true;
        } else if (!qualifier(*predicate.rhsColumn) && !outerTableStack_.empty()) {
            // Unqualified RHS inside a subquery is an outer reference when a scope exists
            // (shape: inner_col = outer_col).
            outer = true;
        }
    }
    if (!outer) {
        return;
    }
    if (nestedUnderCorrelated || outerTableStack_.size() > 1) {
        throw std::runtime_error("multi-level correlated subqueries are not supported");
    }
    predicate.referencesOuter = true;
}

Value Parser::parseValue() {
    if (match(TokenType::Minus)) {
        const auto token = advance();
        if (token.type != TokenType::Number) {
            throw std::runtime_error("expected numeric literal after minus");
        }
        if (token.lexeme.find('.') != std::string::npos) {
            return Value{-std::stod(token.lexeme)};
        }
        return Value{-parser_detail::parseIntLiteral(token.lexeme)};
    }
    const auto token = advance();
    if (token.type == TokenType::Identifier && equalsIgnoreCase(token.lexeme, "NULL")) {
        return Value{};
    }
    if (token.type == TokenType::String) {
        return Value{token.lexeme};
    }
    if (token.type == TokenType::Number) {
        if (token.lexeme.find('.') != std::string::npos) {
            return Value{std::stod(token.lexeme)};
        }
        return Value{parser_detail::parseIntLiteral(token.lexeme)};
    }
    throw std::runtime_error("expected literal value");
}

} // namespace VertexDB
