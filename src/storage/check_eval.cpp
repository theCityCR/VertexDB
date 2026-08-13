#include "VertexDB/storage/check_eval.hpp"

#include "VertexDB/common/comparison_operator.hpp"
#include "VertexDB/common/value.hpp"

#include <functional>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <variant>

namespace VertexDB {
namespace {

enum class TriBool { False, True, Unknown };

using CheckColumnLookup = std::function<std::optional<std::size_t>(std::string_view)>;

bool compareValues(const Value &left, ComparisonOperator op, const Value &right) {
    switch (op) {
    case ComparisonOperator::Equal:
        return left == right;
    case ComparisonOperator::Greater:
        return right < left;
    case ComparisonOperator::Less:
        return left < right;
    }
    return false;
}

std::string literalForCheck(const Value &value) {
    if (value.isNull()) {
        return "NULL";
    }
    switch (value.type()) {
    case ColumnType::Int:
        return value.toString();
    case ColumnType::Double: {
        std::ostringstream out;
        out << std::get<double>(value.data());
        auto text = out.str();
        if (text.find('.') == std::string::npos && text.find('e') == std::string::npos &&
            text.find('E') == std::string::npos) {
            text += ".0";
        }
        return text;
    }
    case ColumnType::String: {
        std::string escaped;
        escaped.reserve(std::get<std::string>(value.data()).size());
        for (const char character : std::get<std::string>(value.data())) {
            if (character == '"' || character == '\\') {
                escaped.push_back('\\');
            }
            escaped.push_back(character);
        }
        return "\"" + escaped + "\"";
    }
    }
    return {};
}

TriBool evalCheckTri(const Predicate &predicate, const Row &row,
                     const CheckColumnLookup &lookup) {
    return std::visit(
        [&](const auto &node) -> TriBool {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, AndPred>) {
                const auto left = evalCheckTri(*node.left, row, lookup);
                if (left == TriBool::False) {
                    return TriBool::False;
                }
                const auto right = evalCheckTri(*node.right, row, lookup);
                if (right == TriBool::False) {
                    return TriBool::False;
                }
                if (left == TriBool::True && right == TriBool::True) {
                    return TriBool::True;
                }
                return TriBool::Unknown;
            } else if constexpr (std::is_same_v<T, OrPred>) {
                const auto left = evalCheckTri(*node.left, row, lookup);
                if (left == TriBool::True) {
                    return TriBool::True;
                }
                const auto right = evalCheckTri(*node.right, row, lookup);
                if (right == TriBool::True) {
                    return TriBool::True;
                }
                if (left == TriBool::False && right == TriBool::False) {
                    return TriBool::False;
                }
                return TriBool::Unknown;
            } else if constexpr (std::is_same_v<T, ComparisonPred>) {
                if (node.expression) {
                    throw std::runtime_error("CHECK does not support expression comparisons");
                }
                const auto leftIndex = lookup(node.column);
                if (!leftIndex) {
                    throw std::runtime_error("unknown CHECK column " + node.column);
                }
                const Value &leftValue = row[*leftIndex];
                if (leftValue.isNull()) {
                    return TriBool::Unknown;
                }
                if (node.rhsColumn) {
                    const auto rightIndex = lookup(*node.rhsColumn);
                    if (!rightIndex) {
                        throw std::runtime_error("unknown CHECK column " + *node.rhsColumn);
                    }
                    const Value &rightValue = row[*rightIndex];
                    if (rightValue.isNull()) {
                        return TriBool::Unknown;
                    }
                    return compareValues(leftValue, node.op, rightValue) ? TriBool::True
                                                                         : TriBool::False;
                }
                if (node.value.isNull()) {
                    return TriBool::Unknown;
                }
                return compareValues(leftValue, node.op, node.value) ? TriBool::True
                                                                    : TriBool::False;
            } else {
                throw std::runtime_error(
                    "CHECK supports only column comparisons combined with AND / OR");
            }
        },
        predicate);
}

void assertSimpleCheckConstraintRec(const Predicate &predicate) {
    std::visit(
        [&](const auto &node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, AndPred> || std::is_same_v<T, OrPred>) {
                assertSimpleCheckConstraintRec(*node.left);
                assertSimpleCheckConstraintRec(*node.right);
            } else if constexpr (std::is_same_v<T, ComparisonPred>) {
                if (node.expression) {
                    throw std::runtime_error("CHECK does not support expression comparisons");
                }
                if (node.referencesOuter) {
                    throw std::runtime_error("CHECK cannot reference outer query columns");
                }
                if (!node.rhsColumn && node.value.isParameter()) {
                    throw std::runtime_error("CHECK does not support parameter placeholders");
                }
            } else {
                throw std::runtime_error(
                    "CHECK supports only column comparisons combined with AND / OR");
            }
        },
        predicate);
}

} // namespace

bool evalCheckPredicate(
    const Predicate &predicate, const Row &row,
    const std::function<std::optional<std::size_t>(std::string_view)> &lookup) {
    return evalCheckTri(predicate, row, lookup) != TriBool::False;
}

void assertSimpleCheckConstraint(const Predicate &predicate) {
    assertSimpleCheckConstraintRec(predicate);
}

std::string checkConstraintLiteral(const Predicate &predicate) {
    return std::visit(
        [&](const auto &node) -> std::string {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, AndPred> || std::is_same_v<T, OrPred>) {
                const auto op = std::is_same_v<T, AndPred> ? " AND " : " OR ";
                return "(" + checkConstraintLiteral(*node.left) + op +
                       checkConstraintLiteral(*node.right) + ")";
            } else if constexpr (std::is_same_v<T, ComparisonPred>) {
                std::string op;
                switch (node.op) {
                case ComparisonOperator::Equal:
                    op = "=";
                    break;
                case ComparisonOperator::Greater:
                    op = ">";
                    break;
                case ComparisonOperator::Less:
                    op = "<";
                    break;
                }
                if (node.rhsColumn) {
                    return node.column + " " + op + " " + *node.rhsColumn;
                }
                return node.column + " " + op + " " + literalForCheck(node.value);
            } else {
                throw std::runtime_error(
                    "CHECK supports only column comparisons combined with AND / OR");
            }
        },
        predicate);
}

} // namespace VertexDB
