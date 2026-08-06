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
    if (predicate.kind == Predicate::Kind::And) {
        return evalPredicate(*predicate.left, row, lookup) &&
               evalPredicate(*predicate.right, row, lookup);
    }
    if (predicate.kind == Predicate::Kind::Or) {
        return evalPredicate(*predicate.left, row, lookup) ||
               evalPredicate(*predicate.right, row, lookup);
    }
    if (predicate.kind == Predicate::Kind::InSubquery) {
        throw std::runtime_error("IN subquery must be materialized before evaluation");
    }
    if (predicate.kind == Predicate::Kind::Exists) {
        throw std::runtime_error("EXISTS must be evaluated by the executor");
    }

    Value leftValue;
    if (predicate.expression) {
        leftValue = evaluateIndexExpression(*predicate.expression, row, lookup);
    } else {
        const auto index = lookup(predicate.column);
        if (!index) {
            throw std::runtime_error("unknown predicate column");
        }
        leftValue = row[*index];
    }

    if (predicate.kind == Predicate::Kind::InList) {
        for (const auto &value : predicate.inValues) {
            if (leftValue == value) {
                return true;
            }
        }
        return false;
    }

    if (predicate.rhsColumn) {
        const auto rightIndex = lookup(*predicate.rhsColumn);
        if (!rightIndex) {
            throw std::runtime_error("unknown predicate column");
        }
        return compareValues(leftValue, predicate.op, row[*rightIndex]);
    }
    return compareValues(leftValue, predicate.op, predicate.value);
}

} // namespace VertexDB
