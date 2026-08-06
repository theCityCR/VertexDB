#include "VertexDB/execution/predicate_eval.hpp"

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

    const auto index = lookup(predicate.column);
    if (!index) {
        throw std::runtime_error("unknown predicate column");
    }
    if (predicate.kind == Predicate::Kind::InList) {
        for (const auto &value : predicate.inValues) {
            if (row[*index] == value) {
                return true;
            }
        }
        return false;
    }
    return compareValues(row[*index], predicate.op, predicate.value);
}

} // namespace VertexDB
