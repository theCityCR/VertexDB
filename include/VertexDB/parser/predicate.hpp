#pragma once

// Recursive SQL predicate variant. Nodes own only fields valid for their shape;
// recursive predicates and subqueries use shared ownership to break type cycles.

#include "VertexDB/common/comparison_operator.hpp"
#include "VertexDB/common/index_expression.hpp"
#include "VertexDB/common/value.hpp"

#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace VertexDB {

struct Select;
struct Predicate;

struct ComparisonPred {
    std::string column;
    ComparisonOperator op{};
    Value value;
    std::optional<std::string> rhsColumn;
    std::optional<IndexExpression> expression;
    bool referencesOuter{false};
};

struct AndPred {
    std::shared_ptr<Predicate> left;
    std::shared_ptr<Predicate> right;
};

struct OrPred {
    std::shared_ptr<Predicate> left;
    std::shared_ptr<Predicate> right;
};

struct InListPred {
    std::string column;
    std::vector<Value> inValues;
    std::optional<IndexExpression> expression;
    bool referencesOuter{false};
};

struct InSubqueryPred {
    std::string column;
    std::shared_ptr<Select> subquery;
    bool referencesOuter{false};
};

struct ExistsPred {
    std::shared_ptr<Select> subquery;
    bool referencesOuter{false};
};

struct Predicate
    : std::variant<ComparisonPred, AndPred, OrPred, InListPred, InSubqueryPred, ExistsPred> {
    using variant::variant;
    using variant::operator=;
};

enum class PredicateKind { Comparison, And, Or, InSubquery, InList, Exists };

[[nodiscard]] inline PredicateKind predicateKind(const Predicate &predicate) {
    return std::visit(
        [](const auto &node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, ComparisonPred>) {
                return PredicateKind::Comparison;
            } else if constexpr (std::is_same_v<T, AndPred>) {
                return PredicateKind::And;
            } else if constexpr (std::is_same_v<T, OrPred>) {
                return PredicateKind::Or;
            } else if constexpr (std::is_same_v<T, InListPred>) {
                return PredicateKind::InList;
            } else if constexpr (std::is_same_v<T, InSubqueryPred>) {
                return PredicateKind::InSubquery;
            } else {
                return PredicateKind::Exists;
            }
        },
        predicate);
}

[[nodiscard]] inline bool predicateReferencesOuter(const Predicate &predicate) {
    return std::visit(
        [](const auto &node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, AndPred> || std::is_same_v<T, OrPred>) {
                return predicateReferencesOuter(*node.left) || predicateReferencesOuter(*node.right);
            } else {
                return node.referencesOuter;
            }
        },
        predicate);
}

[[nodiscard]] inline Predicate makeComparison(std::string column, ComparisonOperator op,
                                              Value value) {
    return ComparisonPred{std::move(column), op, std::move(value)};
}

[[nodiscard]] inline Predicate makeExpressionComparison(IndexExpression expression,
                                                        ComparisonOperator op, Value value) {
    auto column = expression.column;
    return ComparisonPred{std::move(column), op, std::move(value), std::nullopt,
                          std::move(expression)};
}

[[nodiscard]] inline Predicate makeAnd(std::shared_ptr<Predicate> left,
                                      std::shared_ptr<Predicate> right) {
    return AndPred{std::move(left), std::move(right)};
}

[[nodiscard]] inline Predicate makeAnd(Predicate left, Predicate right) {
    return makeAnd(std::make_shared<Predicate>(std::move(left)),
                   std::make_shared<Predicate>(std::move(right)));
}

[[nodiscard]] inline Predicate makeOr(std::shared_ptr<Predicate> left,
                                     std::shared_ptr<Predicate> right) {
    return OrPred{std::move(left), std::move(right)};
}

[[nodiscard]] inline Predicate makeOr(Predicate left, Predicate right) {
    return makeOr(std::make_shared<Predicate>(std::move(left)),
                  std::make_shared<Predicate>(std::move(right)));
}

[[nodiscard]] inline Predicate makeInList(std::string column, std::vector<Value> values) {
    return InListPred{std::move(column), std::move(values)};
}

[[nodiscard]] inline Predicate makeInSubquery(std::string column,
                                             std::shared_ptr<Select> subquery) {
    return InSubqueryPred{std::move(column), std::move(subquery)};
}

[[nodiscard]] inline Predicate makeExists(std::shared_ptr<Select> subquery) {
    return ExistsPred{std::move(subquery)};
}

} // namespace VertexDB
