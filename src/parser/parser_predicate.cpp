#include "VertexDB/parser/parser.hpp"

#include "VertexDB/common/string_utils.hpp"
#include "parse_utils.hpp"

#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>

namespace VertexDB {

using parser_detail::columnQualifier;
using parser_detail::refersToOuterTable;

Predicate Parser::parsePredicate() { return parseOrPredicate(); }

Predicate Parser::parseOrPredicate() {
    auto predicate = parseAndPredicate();
    while (match(TokenType::Identifier, "OR")) {
        auto left = std::make_shared<Predicate>(std::move(predicate));
        auto right = std::make_shared<Predicate>(parseAndPredicate());
        predicate = makeOr(std::move(left), std::move(right));
    }
    return predicate;
}

Predicate Parser::parseAndPredicate() {
    auto predicate = parsePrimaryPredicate();
    while (match(TokenType::Identifier, "AND")) {
        auto left = std::make_shared<Predicate>(std::move(predicate));
        auto right = std::make_shared<Predicate>(parsePrimaryPredicate());
        predicate = makeAnd(std::move(left), std::move(right));
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
                return makeExpressionComparison(std::move(expression), op, parseValue());
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
    auto subquery = std::make_shared<Select>(parseSubquerySelect(true));
    expect(TokenType::RightParen);
    auto predicate = makeExists(std::move(subquery));
    auto &exists = std::get<ExistsPred>(predicate);
    if (exists.subquery && exists.subquery->hasOuterRefs) {
        exists.referencesOuter = true;
    }
    return predicate;
}

IndexExpression Parser::parseIndexExpression() {
    if (match(TokenType::Minus)) {
        const auto column = advance();
        if (column.type != TokenType::Identifier) {
            throw std::runtime_error("expected column after unary minus");
        }
        if (columnQualifier(column.lexeme)) {
            throw std::runtime_error("expression index column must be unqualified");
        }
        return IndexExpression{IndexExpression::Kind::Negate, column.lexeme, {}};
    }

    const auto column = advance();
    if (column.type != TokenType::Identifier) {
        throw std::runtime_error("expected expression column");
    }
    if (equalsIgnoreCase(column.lexeme, "trigram")) {
        expect(TokenType::LeftParen);
        const auto inner = advance();
        if (inner.type != TokenType::Identifier) {
            throw std::runtime_error("expected trigram column");
        }
        if (columnQualifier(inner.lexeme)) {
            throw std::runtime_error("expression index column must be unqualified");
        }
        expect(TokenType::RightParen);
        return IndexExpression{IndexExpression::Kind::Trigram, inner.lexeme, {}};
    }
    if (columnQualifier(column.lexeme)) {
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
        auto subquery = std::make_shared<Select>(parseSubquerySelect(true));
        if (subquery->columns.size() != 1 || isStarProjection(subquery->columns) ||
            subquery->columns.front().kind != SelectExpr::Kind::Column) {
            throw std::runtime_error("IN subquery must project exactly one column");
        }
        expect(TokenType::RightParen);
        Predicate predicate = makeInSubquery(column.lexeme, std::move(subquery));
        auto &inSubquery = std::get<InSubqueryPred>(predicate);
        if (inSubquery.subquery && inSubquery.subquery->hasOuterRefs) {
            inSubquery.referencesOuter = true;
        }
        return predicate;
    }
    if (match(TokenType::Identifier, "LIKE")) {
        const auto pattern = parseValue();
        if (pattern.type() != ColumnType::String || pattern.isNull()) {
            throw std::runtime_error("LIKE pattern must be a string literal");
        }
        return makeLike(column.lexeme, std::get<std::string>(pattern.data()));
    }
    if (match(TokenType::Tilde)) {
        const auto pattern = parseValue();
        if (pattern.type() != ColumnType::String || pattern.isNull()) {
            throw std::runtime_error("regex pattern must be a string literal");
        }
        return makeRegex(column.lexeme, std::get<std::string>(pattern.data()));
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
        return ComparisonPred{column.lexeme, op, {}, rhs.lexeme};
    }
    return makeComparison(column.lexeme, op, parseValue());
}

Value Parser::parseValue() {
    if (match(TokenType::Parameter)) {
        return Value::parameter(nextParameterIndex_++);
    }
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
