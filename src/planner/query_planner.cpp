#include "VertexDB/planner/query_planner.hpp"

#include <algorithm>
#include <memory>
#include <sstream>
#include <utility>

namespace VertexDB {
namespace {

void collectAndConjuncts(const Predicate &predicate, std::vector<const Predicate *> &out) {
    if (predicate.kind == Predicate::Kind::And) {
        collectAndConjuncts(*predicate.left, out);
        collectAndConjuncts(*predicate.right, out);
        return;
    }
    out.push_back(&predicate);
}

std::optional<Predicate> buildAndTree(const std::vector<const Predicate *> &conjuncts) {
    if (conjuncts.empty()) {
        return std::nullopt;
    }
    Predicate tree = *conjuncts.front();
    for (std::size_t i = 1; i < conjuncts.size(); ++i) {
        tree = Predicate{Predicate::Kind::And, std::make_shared<Predicate>(std::move(tree)),
                         std::make_shared<Predicate>(*conjuncts[i])};
    }
    return tree;
}

bool isIndexableComparison(const Predicate &predicate, const Table &table, AccessPath &path,
                           double &cost, std::size_t rowCount) {
    if (predicate.kind != Predicate::Kind::Comparison) {
        return false;
    }
    if (predicate.op == ComparisonOperator::Equal && table.hasIndex(predicate.column)) {
        path = AccessPath::HashIndexLookup;
        cost = 1.0;
        return true;
    }
    if ((predicate.op == ComparisonOperator::Greater ||
         predicate.op == ComparisonOperator::Less) &&
        table.hasOrderedIndex(predicate.column)) {
        path = AccessPath::OrderedIndexRange;
        cost = static_cast<double>(std::max<std::size_t>(rowCount / 3, 1));
        return true;
    }
    return false;
}

bool isIndexableInList(const Predicate &predicate, const Table &table, double &cost) {
    if (predicate.kind != Predicate::Kind::InList) {
        return false;
    }
    if (!table.hasIndex(predicate.column) || predicate.inValues.empty()) {
        return false;
    }
    cost = static_cast<double>(predicate.inValues.size());
    return true;
}

} // namespace

QueryPlan QueryPlanner::planSelect(const Select &query, const Table &table) const {
    QueryPlan plan;
    plan.estimatedRows = table.rowCount();
    plan.estimatedCost = static_cast<double>(std::max<std::size_t>(plan.estimatedRows, 1));

    if (!query.where) {
        return plan;
    }

    // OR trees are not split for indexing in v1.
    if (query.where->kind == Predicate::Kind::Or) {
        plan.residual = query.where;
        plan.explanation = "full table scan (OR predicate)";
        return plan;
    }

    std::vector<const Predicate *> conjuncts;
    collectAndConjuncts(*query.where, conjuncts);

    std::size_t bestIndex = conjuncts.size();
    AccessPath bestPath = AccessPath::FullScan;
    double bestCost = plan.estimatedCost;

    auto consider = [&](std::size_t i, AccessPath path, double cost) {
        // Prefer any index path that is no worse than a full scan; prefer lower cost, then
        // equality over range/IN when tied (more selective by design).
        const bool betterCost = cost < bestCost;
        const bool firstIndexAtParity =
            cost == bestCost && bestPath == AccessPath::FullScan && path != AccessPath::FullScan;
        const bool preferEquality =
            cost == bestCost && path == AccessPath::HashIndexLookup &&
            bestPath != AccessPath::HashIndexLookup && bestPath != AccessPath::FullScan;
        if (betterCost || firstIndexAtParity || preferEquality) {
            bestIndex = i;
            bestPath = path;
            bestCost = cost;
        }
    };

    for (std::size_t i = 0; i < conjuncts.size(); ++i) {
        AccessPath path = AccessPath::FullScan;
        double comparisonCost = bestCost;
        if (isIndexableComparison(*conjuncts[i], table, path, comparisonCost, plan.estimatedRows)) {
            consider(i, path, comparisonCost);
        }
        double inCost = bestCost;
        if (isIndexableInList(*conjuncts[i], table, inCost)) {
            consider(i, AccessPath::HashIndexInLookup, inCost);
        }
    }

    if (bestIndex == conjuncts.size()) {
        plan.residual = query.where;
        return plan;
    }

    const Predicate &chosen = *conjuncts[bestIndex];
    plan.accessPath = bestPath;
    plan.estimatedCost = bestCost;
    plan.indexColumn = chosen.column;

    if (bestPath == AccessPath::HashIndexLookup) {
        plan.indexOp = ComparisonOperator::Equal;
        plan.indexValue = chosen.value;
        plan.estimatedRows = std::min<std::size_t>(plan.estimatedRows, 1);
        plan.explanation = "hash index equality lookup on " + chosen.column;
    } else if (bestPath == AccessPath::OrderedIndexRange) {
        plan.indexOp = chosen.op;
        plan.indexValue = chosen.value;
        plan.estimatedRows = std::max<std::size_t>(plan.estimatedRows / 3, 1);
        plan.explanation = "ordered index range lookup on " + chosen.column;
    } else if (bestPath == AccessPath::HashIndexInLookup) {
        plan.indexOp = ComparisonOperator::Equal;
        plan.indexValues = chosen.inValues;
        plan.estimatedRows = std::min(plan.estimatedRows, plan.indexValues.size());
        plan.explanation = "hash index IN lookup on " + chosen.column + " (" +
                           std::to_string(plan.indexValues.size()) + " values)";
    }

    std::vector<const Predicate *> residualConjuncts;
    residualConjuncts.reserve(conjuncts.size() - 1);
    for (std::size_t i = 0; i < conjuncts.size(); ++i) {
        if (i != bestIndex) {
            residualConjuncts.push_back(conjuncts[i]);
        }
    }
    plan.residual = buildAndTree(residualConjuncts);
    if (plan.residual) {
        plan.notes.push_back("residual filter applied after index lookup");
    }
    return plan;
}

std::string formatPlanExplanation(const QueryPlan &plan) {
    std::ostringstream out;
    out << plan.explanation;
    for (const auto &note : plan.notes) {
        out << "\n" << note;
    }
    if (plan.residual) {
        out << "\nresidual: yes";
    } else if (plan.accessPath != AccessPath::FullScan) {
        out << "\nresidual: no";
    }
    return out.str();
}

} // namespace VertexDB
