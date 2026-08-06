#include "VertexDB/common/index_expression.hpp"

#include "VertexDB/parser/parser.hpp"

#include <stdexcept>
#include <variant>

namespace VertexDB {
namespace {

[[nodiscard]] Value requireNumeric(const Value &value, std::string_view context) {
    if (value.isNull()) {
        throw std::runtime_error(std::string{context} + " cannot be null");
    }
    if (value.type() != ColumnType::Int && value.type() != ColumnType::Double) {
        throw std::runtime_error(std::string{context} + " requires a numeric value");
    }
    return value;
}

[[nodiscard]] Value negateValue(const Value &value) {
    const auto numeric = requireNumeric(value, "unary minus");
    if (numeric.type() == ColumnType::Int) {
        return Value{-std::get<std::int64_t>(numeric.data())};
    }
    return Value{-std::get<double>(numeric.data())};
}

[[nodiscard]] Value addValues(const Value &left, const Value &right) {
    const auto lhs = requireNumeric(left, "addition");
    const auto rhs = requireNumeric(right, "addition");
    if (lhs.type() == ColumnType::Double || rhs.type() == ColumnType::Double) {
        const double leftNumber = lhs.type() == ColumnType::Double
                                      ? std::get<double>(lhs.data())
                                      : static_cast<double>(std::get<std::int64_t>(lhs.data()));
        const double rightNumber = rhs.type() == ColumnType::Double
                                       ? std::get<double>(rhs.data())
                                       : static_cast<double>(std::get<std::int64_t>(rhs.data()));
        return Value{leftNumber + rightNumber};
    }
    return Value{std::get<std::int64_t>(lhs.data()) + std::get<std::int64_t>(rhs.data())};
}

[[nodiscard]] Value subtractValues(const Value &left, const Value &right) {
    const auto lhs = requireNumeric(left, "subtraction");
    const auto rhs = requireNumeric(right, "subtraction");
    if (lhs.type() == ColumnType::Double || rhs.type() == ColumnType::Double) {
        const double leftNumber = lhs.type() == ColumnType::Double
                                      ? std::get<double>(lhs.data())
                                      : static_cast<double>(std::get<std::int64_t>(lhs.data()));
        const double rightNumber = rhs.type() == ColumnType::Double
                                       ? std::get<double>(rhs.data())
                                       : static_cast<double>(std::get<std::int64_t>(rhs.data()));
        return Value{leftNumber - rightNumber};
    }
    return Value{std::get<std::int64_t>(lhs.data()) - std::get<std::int64_t>(rhs.data())};
}

} // namespace

std::string indexExpressionToString(const IndexExpression &expression) {
    switch (expression.kind) {
    case IndexExpression::Kind::Column:
        return expression.column;
    case IndexExpression::Kind::Negate:
        return "-" + expression.column;
    case IndexExpression::Kind::Add:
        return expression.column + "+" + expression.literal.toString();
    case IndexExpression::Kind::Subtract:
        return expression.column + "-" + expression.literal.toString();
    }
    return {};
}

std::optional<IndexExpression> parseIndexExpressionString(std::string_view text) {
    try {
        Parser parser;
        const auto query = parser.parse("CREATE INDEX __expr ON __t ((" + std::string{text} + "));");
        if (!std::holds_alternative<CreateIndex>(query)) {
            return std::nullopt;
        }
        return std::get<CreateIndex>(query).expression;
    } catch (const std::runtime_error &) {
        return std::nullopt;
    }
}

Value evaluateIndexExpression(
    const IndexExpression &expression, const Row &row,
    const std::function<std::optional<std::size_t>(std::string_view)> &lookup) {
    const auto index = lookup(expression.column);
    if (!index) {
        throw std::runtime_error("unknown expression index column");
    }
    const Value &base = row[*index];
    switch (expression.kind) {
    case IndexExpression::Kind::Column:
        return base;
    case IndexExpression::Kind::Negate:
        return negateValue(base);
    case IndexExpression::Kind::Add:
        return addValues(base, expression.literal);
    case IndexExpression::Kind::Subtract:
        return subtractValues(base, expression.literal);
    }
    throw std::runtime_error("unsupported index expression");
}

std::string encodeIndexDefinitionColumn(const std::string &column,
                                        const std::optional<IndexExpression> &expression) {
    if (!expression) {
        return column;
    }
    return std::string{kExpressionIndexPrefix} + indexExpressionToString(*expression);
}

std::pair<std::string, std::optional<IndexExpression>>
decodeIndexDefinitionColumn(std::string_view encoded) {
    if (encoded.substr(0, kExpressionIndexPrefix.size()) == kExpressionIndexPrefix) {
        const auto text = encoded.substr(kExpressionIndexPrefix.size());
        auto expression = parseIndexExpressionString(text);
        if (!expression) {
            throw std::runtime_error("invalid expression index metadata");
        }
        return {expression->column, std::move(expression)};
    }
    return {std::string{encoded}, std::nullopt};
}

} // namespace VertexDB
