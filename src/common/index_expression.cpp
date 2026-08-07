#include "VertexDB/common/index_expression.hpp"

#include <cctype>
#include <charconv>
#include <stdexcept>
#include <string>
#include <utility>
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

[[nodiscard]] bool isIdentStart(char ch) {
    return std::isalpha(static_cast<unsigned char>(ch)) != 0 || ch == '_';
}

[[nodiscard]] bool isIdentContinue(char ch) {
    return std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '_';
}

[[nodiscard]] std::string_view trim(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0) {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0) {
        text.remove_suffix(1);
    }
    return text;
}

[[nodiscard]] bool tryParseIdentifier(std::string_view text, std::string &out) {
    if (text.empty() || !isIdentStart(text.front())) {
        return false;
    }
    std::size_t end = 1;
    while (end < text.size() && isIdentContinue(text[end])) {
        ++end;
    }
    if (end != text.size()) {
        return false;
    }
    out.assign(text.data(), end);
    return true;
}

[[nodiscard]] std::optional<Value> parseExpressionLiteral(std::string_view text) {
    text = trim(text);
    if (text.empty()) {
        return std::nullopt;
    }
    if (text == "NULL") {
        return Value{};
    }

    const bool negative = text.front() == '-';
    std::string_view digits = negative ? text.substr(1) : text;
    if (digits.empty()) {
        return std::nullopt;
    }

    const bool looksNumeric =
        std::isdigit(static_cast<unsigned char>(digits.front())) != 0 || digits.front() == '.';
    if (looksNumeric) {
        if (digits.find('.') != std::string_view::npos) {
            try {
                const double value = std::stod(std::string{digits});
                return Value{negative ? -value : value};
            } catch (const std::exception &) {
                return std::nullopt;
            }
        }
        std::int64_t value{};
        const auto [ptr, ec] =
            std::from_chars(digits.data(), digits.data() + digits.size(), value);
        if (ec != std::errc{} || ptr != digits.data() + digits.size()) {
            return std::nullopt;
        }
        return Value{negative ? -value : value};
    }

    // Unquoted string remnant from Value::toString() round-trip.
    return Value{std::string{text}};
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
    text = trim(text);
    if (text.empty()) {
        return std::nullopt;
    }

    // -column
    if (text.front() == '-') {
        std::string column;
        if (!tryParseIdentifier(text.substr(1), column)) {
            return std::nullopt;
        }
        return IndexExpression{IndexExpression::Kind::Negate, std::move(column), {}};
    }

    std::size_t identEnd = 0;
    if (!isIdentStart(text.front())) {
        return std::nullopt;
    }
    ++identEnd;
    while (identEnd < text.size() && isIdentContinue(text[identEnd])) {
        ++identEnd;
    }
    std::string column{text.substr(0, identEnd)};
    const auto rest = text.substr(identEnd);

    if (rest.empty()) {
        return IndexExpression{IndexExpression::Kind::Column, std::move(column), {}};
    }
    if (rest.front() == '+') {
        auto literal = parseExpressionLiteral(rest.substr(1));
        if (!literal) {
            return std::nullopt;
        }
        return IndexExpression{IndexExpression::Kind::Add, std::move(column), std::move(*literal)};
    }
    if (rest.front() == '-') {
        auto literal = parseExpressionLiteral(rest.substr(1));
        if (!literal) {
            return std::nullopt;
        }
        return IndexExpression{IndexExpression::Kind::Subtract, std::move(column),
                               std::move(*literal)};
    }
    return std::nullopt;
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
