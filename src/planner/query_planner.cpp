#include "VertexDB/planner/query_planner.hpp"

#include <algorithm>

namespace VertexDB {
namespace {

const Predicate *simpleComparison(const std::optional<Predicate> &predicate) {
    if (!predicate || predicate->kind != Predicate::Kind::Comparison) {
        return nullptr;
    }
    return &*predicate;
}

} // namespace

QueryPlan QueryPlanner::planSelect(const Select &query, const Table &table) const {
    QueryPlan plan;
    plan.predicate = query.where;
    plan.estimatedRows = table.rowCount();
    plan.estimatedCost = static_cast<double>(std::max<std::size_t>(plan.estimatedRows, 1));
    const auto *predicate = simpleComparison(query.where);
    if (predicate == nullptr) {
        return plan;
    }

    if (predicate->op == ComparisonOperator::Equal && table.hasIndex(predicate->column)) {
        plan.accessPath = AccessPath::HashIndexLookup;
        plan.estimatedRows = std::min<std::size_t>(plan.estimatedRows, 1);
        plan.estimatedCost = 1.0;
        plan.explanation = "hash index equality lookup";
        return plan;
    }

    if ((predicate->op == ComparisonOperator::Greater ||
         predicate->op == ComparisonOperator::Less) &&
        table.hasOrderedIndex(predicate->column)) {
        plan.accessPath = AccessPath::OrderedIndexRange;
        plan.estimatedRows = std::max<std::size_t>(plan.estimatedRows / 3, 1);
        plan.estimatedCost = static_cast<double>(plan.estimatedRows);
        plan.explanation = "ordered index range lookup";
        return plan;
    }

    return plan;
}

} // namespace VertexDB
