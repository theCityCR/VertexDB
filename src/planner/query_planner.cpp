#include "VertexDB/planner/query_planner.hpp"

#include "VertexDB/common/index_expression.hpp"
#include "VertexDB/storage/histogram.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <sstream>
#include <string>
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

[[nodiscard]] double averageRowsPerKey(std::size_t rowCount, std::size_t distinctKeys) {
    if (rowCount == 0) {
        return 1.0;
    }
    const auto distinct = std::max<std::size_t>(distinctKeys, 1);
    return static_cast<double>(rowCount) / static_cast<double>(distinct);
}

[[nodiscard]] std::size_t distinctOrOne(const Table &table, std::string_view column) {
    if (const auto distinct = table.indexDistinctCount(column)) {
        return std::max<std::size_t>(*distinct, 1);
    }
    if (const auto histogram = table.columnHistogram(column)) {
        return std::max<std::size_t>(static_cast<std::size_t>(histogram->distinctCount), 1);
    }
    return 1;
}

[[nodiscard]] std::size_t distinctOrOne(const Table &table, const IndexExpression &expression) {
    if (const auto distinct = table.indexDistinctCount(expression)) {
        return std::max<std::size_t>(*distinct, 1);
    }
    return 1;
}

[[nodiscard]] double rangeCost(const Table &table, std::string_view column, ComparisonOperator op,
                               const Value &value, std::size_t rowCount) {
    if (const auto histogram = table.columnHistogram(column)) {
        const double selectivity = histogramRangeSelectivity(*histogram, op, value);
        return std::max(selectivity * static_cast<double>(std::max<std::size_t>(rowCount, 1)), 1.0);
    }
    return std::max(static_cast<double>(rowCount) / 3.0, 1.0);
}

[[nodiscard]] double inCost(const Table &table, std::string_view column,
                            const std::vector<Value> &values, std::size_t rowCount) {
    if (const auto histogram = table.columnHistogram(column)) {
        const double selectivity = histogramInSelectivity(*histogram, values);
        return std::max(selectivity * static_cast<double>(std::max<std::size_t>(rowCount, 1)), 1.0);
    }
    return static_cast<double>(values.size()) * averageRowsPerKey(rowCount, distinctOrOne(table, column));
}

bool isIndexableComparison(const Predicate &predicate, const Table &table, AccessPath &path,
                           double &cost, std::size_t rowCount) {
    if (predicate.kind != Predicate::Kind::Comparison || predicate.rhsColumn) {
        return false;
    }
    if (predicate.expression) {
        if (!table.hasExpressionIndex(*predicate.expression)) {
            return false;
        }
        if (predicate.op == ComparisonOperator::Equal) {
            path = AccessPath::HashIndexLookup;
            cost = averageRowsPerKey(rowCount, distinctOrOne(table, *predicate.expression));
            return true;
        }
        if (predicate.op == ComparisonOperator::Greater ||
            predicate.op == ComparisonOperator::Less) {
            path = AccessPath::OrderedIndexRange;
            // Expression indexes have no column histogram; keep the N/3 fallback.
            cost = std::max(static_cast<double>(rowCount) / 3.0, 1.0);
            return true;
        }
        return false;
    }
    if (predicate.op == ComparisonOperator::Equal && table.hasIndex(predicate.column)) {
        path = AccessPath::HashIndexLookup;
        cost = averageRowsPerKey(rowCount, distinctOrOne(table, predicate.column));
        return true;
    }
    if ((predicate.op == ComparisonOperator::Greater ||
         predicate.op == ComparisonOperator::Less) &&
        table.hasIndex(predicate.column)) {
        path = AccessPath::OrderedIndexRange;
        cost = rangeCost(table, predicate.column, predicate.op, predicate.value, rowCount);
        return true;
    }
    return false;
}

bool isIndexableInList(const Predicate &predicate, const Table &table, double &cost,
                       std::size_t rowCount) {
    if (predicate.kind != Predicate::Kind::InList) {
        return false;
    }
    if (!table.hasIndex(predicate.column) || predicate.inValues.empty()) {
        return false;
    }
    cost = inCost(table, predicate.column, predicate.inValues, rowCount);
    return true;
}

bool isEqualityIndexProbe(const Predicate &predicate, const Table &table) {
    if (predicate.kind != Predicate::Kind::Comparison || predicate.rhsColumn) {
        return false;
    }
    if (predicate.op != ComparisonOperator::Equal) {
        return false;
    }
    if (predicate.expression) {
        return table.hasExpressionIndex(*predicate.expression);
    }
    return table.hasIndex(predicate.column);
}

[[nodiscard]] IndexEqualityProbe makeEqualityProbe(const Predicate &predicate) {
    IndexEqualityProbe probe;
    probe.column = predicate.column;
    probe.expression = predicate.expression;
    probe.value = predicate.value;
    return probe;
}

[[nodiscard]] double equalityFanout(const Predicate &predicate, const Table &table,
                                    std::size_t rowCount) {
    if (predicate.expression) {
        return averageRowsPerKey(rowCount, distinctOrOne(table, *predicate.expression));
    }
    return averageRowsPerKey(rowCount, distinctOrOne(table, predicate.column));
}

[[nodiscard]] std::size_t rowsFromCost(double cost, std::size_t rowCount) {
    if (rowCount == 0) {
        return 0;
    }
    return std::min(rowCount, static_cast<std::size_t>(std::max(std::llround(cost), 1LL)));
}

[[nodiscard]] std::string probeLabel(const IndexEqualityProbe &probe) {
    if (probe.expression) {
        return "(" + indexExpressionToString(*probe.expression) + ")";
    }
    return probe.column;
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
        double inListCost = bestCost;
        if (isIndexableInList(*conjuncts[i], table, inListCost, plan.estimatedRows)) {
            consider(i, AccessPath::HashIndexInLookup, inListCost);
        }
    }

    std::vector<std::size_t> equalityIndexes;
    for (std::size_t i = 0; i < conjuncts.size(); ++i) {
        if (isEqualityIndexProbe(*conjuncts[i], table)) {
            equalityIndexes.push_back(i);
        }
    }

    bool choseIntersect = false;
    double intersectCost = bestCost;
    double intersectRows = 0.0;
    if (equalityIndexes.size() >= 2) {
        // Independence: N * Π(1/D_i). Cheaper than the best single index when combination is more
        // selective than any one conjunct alone.
        intersectRows = static_cast<double>(std::max<std::size_t>(plan.estimatedRows, 1));
        for (const auto index : equalityIndexes) {
            const double fanout = equalityFanout(*conjuncts[index], table, plan.estimatedRows);
            const double selectivity =
                fanout / static_cast<double>(std::max<std::size_t>(plan.estimatedRows, 1));
            intersectRows *= std::clamp(selectivity, 0.0, 1.0);
        }
        intersectCost = std::max(intersectRows, 1.0);
        if (intersectCost < bestCost) {
            choseIntersect = true;
        }
    }

    if (!choseIntersect && bestIndex == conjuncts.size()) {
        plan.residual = query.where;
        return plan;
    }

    if (choseIntersect) {
        plan.accessPath = AccessPath::MultiIndexIntersect;
        plan.estimatedCost = intersectCost;
        plan.estimatedRows = rowsFromCost(intersectCost, table.rowCount());
        plan.intersectProbes.reserve(equalityIndexes.size());
        std::ostringstream labels;
        std::vector<const Predicate *> residualConjuncts;
        residualConjuncts.reserve(conjuncts.size());
        for (std::size_t i = 0; i < conjuncts.size(); ++i) {
            const bool used =
                std::find(equalityIndexes.begin(), equalityIndexes.end(), i) != equalityIndexes.end();
            if (used) {
                auto probe = makeEqualityProbe(*conjuncts[i]);
                if (!labels.str().empty()) {
                    labels << ", ";
                }
                labels << probeLabel(probe);
                plan.intersectProbes.push_back(std::move(probe));
            } else {
                residualConjuncts.push_back(conjuncts[i]);
            }
        }
        plan.explanation = "multi-index intersect on " + labels.str();
        plan.residual = buildAndTree(residualConjuncts);
        if (plan.residual) {
            plan.notes.push_back("residual filter applied after index lookup");
        }
        return plan;
    }

    const Predicate &chosen = *conjuncts[bestIndex];
    plan.accessPath = bestPath;
    plan.estimatedCost = bestCost;
    plan.indexColumn = chosen.column;
    plan.indexExpression = chosen.expression;

    if (bestPath == AccessPath::HashIndexLookup) {
        plan.indexOp = ComparisonOperator::Equal;
        plan.indexValue = chosen.value;
        plan.estimatedRows = rowsFromCost(bestCost, table.rowCount());
        if (chosen.expression) {
            plan.explanation = "expression hash index equality lookup on (" +
                               indexExpressionToString(*chosen.expression) + ")";
        } else {
            plan.explanation = "hash index equality lookup on " + chosen.column;
        }
    } else if (bestPath == AccessPath::OrderedIndexRange) {
        plan.indexOp = chosen.op;
        plan.indexValue = chosen.value;
        plan.estimatedRows = rowsFromCost(bestCost, table.rowCount());
        if (chosen.expression) {
            plan.explanation = "expression ordered index range lookup on (" +
                               indexExpressionToString(*chosen.expression) + ")";
        } else {
            plan.explanation = "ordered index range lookup on " + chosen.column;
        }
    } else if (bestPath == AccessPath::HashIndexInLookup) {
        plan.indexOp = ComparisonOperator::Equal;
        plan.indexValues = chosen.inValues;
        plan.estimatedRows = rowsFromCost(bestCost, table.rowCount());
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

JoinPlan QueryPlanner::planJoin(const Table &left, const Table &right,
                                const JoinClause &join) const {
    JoinPlan plan;
    const auto leftRows = left.rowCount();
    const auto rightRows = right.rowCount();
    plan.estimatedRows = std::max(leftRows, rightRows);
    // Build hash table on the right, probe from the left (matches the executor's hash join).
    plan.estimatedCost =
        static_cast<double>(std::max<std::size_t>(leftRows, 1) + std::max<std::size_t>(rightRows, 1));
    plan.explanation = "hash join";
    plan.algorithm = JoinAlgorithm::HashJoin;
    plan.outerIsLeft = true;

    const bool rightIndexed = right.hasIndex(join.rightColumn);
    const bool leftIndexed = left.hasIndex(join.leftColumn);

    auto considerNested = [&](bool scanLeft, double cost, std::string_view probeTable,
                              std::string_view probeColumn) {
        if (cost < plan.estimatedCost) {
            plan.algorithm = JoinAlgorithm::NestedLoopIndexProbe;
            plan.estimatedCost = cost;
            plan.outerIsLeft = scanLeft;
            plan.probeTable = std::string{probeTable};
            plan.probeColumn = std::string{probeColumn};
            plan.explanation = std::string{"nested loop join (index probe on "} +
                               std::string{probeTable} + "." + std::string{probeColumn} + ")";
        }
    };

    if (rightIndexed) {
        const double fanout = averageRowsPerKey(rightRows, distinctOrOne(right, join.rightColumn));
        considerNested(true, static_cast<double>(std::max<std::size_t>(leftRows, 1)) * fanout,
                       right.name(), join.rightColumn);
    }
    if (leftIndexed) {
        const double fanout = averageRowsPerKey(leftRows, distinctOrOne(left, join.leftColumn));
        considerNested(false, static_cast<double>(std::max<std::size_t>(rightRows, 1)) * fanout,
                       left.name(), join.leftColumn);
    }

    return plan;
}

JoinPlan QueryPlanner::planJoinAgainstRows(std::size_t leftRows, const Table &right,
                                           const JoinClause &join) const {
    JoinPlan plan;
    const auto rightRows = right.rowCount();
    plan.estimatedRows = std::max(leftRows, rightRows);
    plan.estimatedCost =
        static_cast<double>(std::max<std::size_t>(leftRows, 1) + std::max<std::size_t>(rightRows, 1));
    plan.explanation = "hash join";
    plan.algorithm = JoinAlgorithm::HashJoin;
    plan.outerIsLeft = true;

    if (right.hasIndex(join.rightColumn)) {
        const double fanout = averageRowsPerKey(rightRows, distinctOrOne(right, join.rightColumn));
        const double cost = static_cast<double>(std::max<std::size_t>(leftRows, 1)) * fanout;
        if (cost < plan.estimatedCost) {
            plan.algorithm = JoinAlgorithm::NestedLoopIndexProbe;
            plan.estimatedCost = cost;
            plan.outerIsLeft = true;
            plan.probeTable = right.name();
            plan.probeColumn = join.rightColumn;
            plan.explanation = std::string{"nested loop join (index probe on "} + right.name() +
                               "." + join.rightColumn + ")";
        }
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
    out << "\nest_rows=" << plan.estimatedRows << " cost=" << plan.estimatedCost;
    return out.str();
}

std::string formatJoinPlanExplanation(const JoinPlan &plan) {
    std::ostringstream out;
    out << plan.explanation;
    out << "\nest_rows=" << plan.estimatedRows << " cost=" << plan.estimatedCost;
    return out.str();
}

} // namespace VertexDB
