#include "VertexDB/execution/predicate_eval.hpp"

#include "VertexDB/common/index_expression.hpp"

#include <stdexcept>

namespace VertexDB {

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

bool evalPredicate(const Predicate &predicate, const Row &row, const ColumnLookup &lookup) {
    return std::visit(
        [&](const auto &node) -> bool {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, AndPred>) {
                return evalPredicate(*node.left, row, lookup) &&
                       evalPredicate(*node.right, row, lookup);
            } else if constexpr (std::is_same_v<T, OrPred>) {
                return evalPredicate(*node.left, row, lookup) ||
                       evalPredicate(*node.right, row, lookup);
            } else if constexpr (std::is_same_v<T, InSubqueryPred>) {
                throw std::runtime_error("IN subquery must be materialized before evaluation");
            } else if constexpr (std::is_same_v<T, ExistsPred>) {
                throw std::runtime_error("EXISTS must be evaluated by the executor");
            } else {
                Value leftValue;
                if (node.expression) {
                    leftValue = evaluateIndexExpression(*node.expression, row, lookup);
                } else {
                    const auto index = lookup(node.column);
                    if (!index) {
                        throw std::runtime_error("unknown predicate column");
                    }
                    leftValue = row[*index];
                }
                if constexpr (std::is_same_v<T, InListPred>) {
                    for (const auto &value : node.inValues) {
                        if (leftValue == value) {
                            return true;
                        }
                    }
                    return false;
                } else {
                    if (node.rhsColumn) {
                        const auto rightIndex = lookup(*node.rhsColumn);
                        if (!rightIndex) {
                            throw std::runtime_error("unknown predicate column");
                        }
                        return compareValues(leftValue, node.op, row[*rightIndex]);
                    }
                    return compareValues(leftValue, node.op, node.value);
                }
            }
        },
        predicate);
}

} // namespace VertexDB
