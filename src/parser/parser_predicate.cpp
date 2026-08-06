#include "VertexDB/parser/parser.hpp"

#include "VertexDB/common/string_utils.hpp"
#include "parse_utils.hpp"

#include <memory>
#include <stdexcept>

namespace VertexDB {
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
    if (match(TokenType::LeftParen)) {
        auto predicate = parsePredicate();
        expect(TokenType::RightParen);
        return predicate;
    }
    return parseComparisonPredicate();
}

Predicate Parser::parseComparisonPredicate() {
    const auto column = advance();
    if (column.type != TokenType::Identifier) {
        throw std::runtime_error("expected predicate column");
    }
    if (match(TokenType::Identifier, "IN")) {
        expect(TokenType::LeftParen);
        expect(TokenType::Identifier, "SELECT");
        auto subquery = std::make_shared<Select>(parseSelectAfterSelectKeyword());
        if (!subquery->ctes.empty()) {
            throw std::runtime_error("WITH inside IN subquery is not supported");
        }
        if (subquery->join) {
            throw std::runtime_error("JOIN inside IN subquery is not supported");
        }
        if (subquery->columns.size() != 1 || subquery->columns.front() == "*") {
            throw std::runtime_error("IN subquery must project exactly one column");
        }
        expect(TokenType::RightParen);
        return Predicate{column.lexeme, std::move(subquery)};
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
    return {column.lexeme, op, parseValue()};
}

Value Parser::parseValue() {
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
