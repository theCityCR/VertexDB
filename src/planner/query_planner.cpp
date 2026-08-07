#include "VertexDB/planner/query_planner.hpp"

#include "VertexDB/common/index_expression.hpp"
#include "VertexDB/storage/histogram.hpp"
#include "VertexDB/storage/table.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <sstream>
#include <string>
#include <utility>

namespace VertexDB {
namespace {

void collectAndConjuncts(const Predicate &predicate, std::vector<const Predicate *> &out) {
    if (const auto *andPred = std::get_if<AndPred>(&predicate)) {
        collectAndConjuncts(*andPred->left, out);
        collectAndConjuncts(*andPred->right, out);
        return;
    }
    out.push_back(&predicate);
}

void collectOrDisjuncts(const Predicate &predicate, std::vector<const Predicate *> &out) {
    if (const auto *orPred = std::get_if<OrPred>(&predicate)) {
        collectOrDisjuncts(*orPred->left, out);
        collectOrDisjuncts(*orPred->right, out);
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
        tree = makeAnd(std::move(tree), *conjuncts[i]);
    }
    return tree;
}

std::optional<Predicate> buildOrTree(const std::vector<const Predicate *> &disjuncts) {
    if (disjuncts.empty()) {
        return std::nullopt;
    }
    Predicate tree = *disjuncts.front();
    for (std::size_t i = 1; i < disjuncts.size(); ++i) {
        tree = makeOr(std::move(tree), *disjuncts[i]);
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

[[nodiscard]] std::size_t distinctOrOne(const RelationStats &stats,
                                        const IndexCatalogView &indexes,
                                        std::string_view column) {
    if (const auto distinct = indexes.indexDistinctCount(column)) {
        return std::max<std::size_t>(*distinct, 1);
    }
    if (const auto histogram = stats.columnHistogram(column)) {
        return std::max<std::size_t>(static_cast<std::size_t>(histogram->distinctCount), 1);
    }
    return 1;
}

[[nodiscard]] std::size_t distinctOrOne(const IndexCatalogView &indexes,
                                        const IndexExpression &expression) {
    if (const auto distinct = indexes.indexDistinctCount(expression)) {
        return std::max<std::size_t>(*distinct, 1);
    }
    return 1;
}

[[nodiscard]] double rangeCost(const RelationStats &stats, std::string_view column,
                               ComparisonOperator op,
                               const Value &value, std::size_t rowCount) {
    if (const auto histogram = stats.columnHistogram(column)) {
        const double selectivity = histogramRangeSelectivity(*histogram, op, value);
        return std::max(selectivity * static_cast<double>(std::max<std::size_t>(rowCount, 1)), 1.0);
    }
    return std::max(static_cast<double>(rowCount) / 3.0, 1.0);
}

[[nodiscard]] double inCost(const RelationStats &stats, const IndexCatalogView &indexes,
                            std::string_view column,
                            const std::vector<Value> &values, std::size_t rowCount) {
    if (const auto histogram = stats.columnHistogram(column)) {
        const double selectivity = histogramInSelectivity(*histogram, values);
        return std::max(selectivity * static_cast<double>(std::max<std::size_t>(rowCount, 1)), 1.0);
    }
    return static_cast<double>(values.size()) *
           averageRowsPerKey(rowCount, distinctOrOne(stats, indexes, column));
}

bool isIndexableComparison(const Predicate &predicate, const RelationStats &stats,
                           const IndexCatalogView &indexes, AccessPath &path, double &cost,
                           std::size_t rowCount) {
    const auto *comparison = std::get_if<ComparisonPred>(&predicate);
    if (comparison == nullptr || comparison->rhsColumn) {
        return false;
    }
    if (comparison->expression) {
        if (!indexes.hasExpressionIndex(*comparison->expression)) {
            return false;
        }
        if (comparison->op == ComparisonOperator::Equal) {
            path = AccessPath::HashIndexLookup;
            cost = averageRowsPerKey(rowCount, distinctOrOne(indexes, *comparison->expression));
            return true;
        }
        if (comparison->op == ComparisonOperator::Greater ||
            comparison->op == ComparisonOperator::Less) {
            path = AccessPath::OrderedIndexRange;
            // Expression indexes have no column histogram; keep the N/3 fallback.
            cost = std::max(static_cast<double>(rowCount) / 3.0, 1.0);
            return true;
        }
        return false;
    }
    if (comparison->op == ComparisonOperator::Equal && indexes.hasIndex(comparison->column)) {
        path = AccessPath::HashIndexLookup;
        cost = averageRowsPerKey(rowCount, distinctOrOne(stats, indexes, comparison->column));
        return true;
    }
    if ((comparison->op == ComparisonOperator::Greater ||
         comparison->op == ComparisonOperator::Less) &&
        indexes.hasIndex(comparison->column)) {
        path = AccessPath::OrderedIndexRange;
        cost = rangeCost(stats, comparison->column, comparison->op, comparison->value, rowCount);
        return true;
    }
    return false;
}

bool isIndexableInList(const Predicate &predicate, const RelationStats &stats,
                       const IndexCatalogView &indexes, double &cost, std::size_t rowCount) {
    const auto *inList = std::get_if<InListPred>(&predicate);
    if (inList == nullptr) {
        return false;
    }
    if (!indexes.hasIndex(inList->column) || inList->inValues.empty()) {
        return false;
    }
    cost = inCost(stats, indexes, inList->column, inList->inValues, rowCount);
    return true;
}

bool isEqualityIndexProbe(const Predicate &predicate, const IndexCatalogView &indexes) {
    const auto *comparison = std::get_if<ComparisonPred>(&predicate);
    if (comparison == nullptr || comparison->rhsColumn) {
        return false;
    }
    if (comparison->op != ComparisonOperator::Equal) {
        return false;
    }
    if (comparison->expression) {
        return indexes.hasExpressionIndex(*comparison->expression);
    }
    return indexes.hasIndex(comparison->column);
}

[[nodiscard]] IndexEqualityProbe makeEqualityProbe(const Predicate &predicate) {
    const auto &comparison = std::get<ComparisonPred>(predicate);
    IndexEqualityProbe probe;
    probe.column = comparison.column;
    probe.expression = comparison.expression;
    probe.value = comparison.value;
    return probe;
}

[[nodiscard]] double equalityFanout(const Predicate &predicate, const RelationStats &stats,
                                    const IndexCatalogView &indexes,
                                    std::size_t rowCount) {
    const auto &comparison = std::get<ComparisonPred>(predicate);
    if (comparison.expression) {
        return averageRowsPerKey(rowCount, distinctOrOne(indexes, *comparison.expression));
    }
    return averageRowsPerKey(rowCount, distinctOrOne(stats, indexes, comparison.column));
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

AccessPath QueryPlan::accessPath() const {
    return std::visit(
        [](const auto &path) {
            using T = std::decay_t<decltype(path)>;
            if constexpr (std::is_same_v<T, FullScanPlan>) {
                return AccessPath::FullScan;
            } else if constexpr (std::is_same_v<T, HashEqPlan>) {
                return AccessPath::HashIndexLookup;
            } else if constexpr (std::is_same_v<T, OrderedRangePlan>) {
                return AccessPath::OrderedIndexRange;
            } else if constexpr (std::is_same_v<T, HashInPlan>) {
                return AccessPath::HashIndexInLookup;
            } else if constexpr (std::is_same_v<T, IntersectPlan>) {
                return AccessPath::MultiIndexIntersect;
            } else {
                return AccessPath::MultiIndexUnion;
            }
        },
        path);
}

QueryPlan QueryPlanner::planSelect(const Select &query, const RelationStats &stats,
                                   const IndexCatalogView &indexes) const {
    QueryPlan plan;
    plan.estimates.estimatedRows = stats.rowCount();
    plan.estimates.estimatedCost =
        static_cast<double>(std::max<std::size_t>(plan.estimates.estimatedRows, 1));

    if (!query.where) {
        return plan;
    }

    // Top-level OR: union equality index probes; non-indexable arms become a residual OR
    // complementary scan (partial OR) when the indexable subset is cheaper than a full scan.
    if (std::holds_alternative<OrPred>(*query.where)) {
        std::vector<const Predicate *> disjuncts;
        collectOrDisjuncts(*query.where, disjuncts);

        std::vector<const Predicate *> indexable;
        std::vector<const Predicate *> residualDisjuncts;
        indexable.reserve(disjuncts.size());
        residualDisjuncts.reserve(disjuncts.size());
        for (const Predicate *pred : disjuncts) {
            if (isEqualityIndexProbe(*pred, indexes)) {
                indexable.push_back(pred);
            } else {
                residualDisjuncts.push_back(pred);
            }
        }

        if (indexable.empty()) {
            plan.estimates.residual = query.where;
            plan.estimates.explanation = "full table scan (OR predicate)";
            return plan;
        }

        // Path choice uses indexable arms only (independence: N * (1 - Π(1 - s_i))).
        const double N =
            static_cast<double>(std::max<std::size_t>(plan.estimates.estimatedRows, 1));
        double indexMissProduct = 1.0;
        for (const Predicate *pred : indexable) {
            const double fanout = equalityFanout(*pred, stats, indexes, plan.estimates.estimatedRows);
            const double selectivity = std::clamp(fanout / N, 0.0, 1.0);
            indexMissProduct *= 1.0 - selectivity;
        }
        const double indexUnionCost = std::max(N * (1.0 - indexMissProduct), 1.0);
        if (!(indexUnionCost < plan.estimates.estimatedCost)) {
            plan.estimates.residual = query.where;
            plan.estimates.explanation = "full table scan (OR predicate)";
            return plan;
        }

        // Estimated rows fold residual arms under independence when stats exist; unknown
        // residual equality defaults to moderately selective so EXPLAIN is not forced to N.
        double missProduct = indexMissProduct;
        for (const Predicate *pred : residualDisjuncts) {
            double selectivity = 0.5;
            if (const auto *comparison = std::get_if<ComparisonPred>(pred);
                comparison != nullptr && !comparison->rhsColumn &&
                comparison->op == ComparisonOperator::Equal) {
                const bool haveDistinct =
                    comparison->expression
                        ? indexes.indexDistinctCount(*comparison->expression).has_value()
                        : (indexes.indexDistinctCount(comparison->column).has_value() ||
                           stats.columnHistogram(comparison->column).has_value());
                if (haveDistinct) {
                    selectivity = std::clamp(
                        equalityFanout(*pred, stats, indexes, plan.estimates.estimatedRows) / N, 0.0,
                        1.0);
                } else {
                    selectivity = 0.1;
                }
            } else if (const auto *comparison = std::get_if<ComparisonPred>(pred);
                       comparison != nullptr && !comparison->rhsColumn &&
                       !comparison->expression &&
                       (comparison->op == ComparisonOperator::Less ||
                        comparison->op == ComparisonOperator::Greater) &&
                       stats.columnHistogram(comparison->column)) {
                selectivity = std::clamp(
                    rangeCost(stats, comparison->column, comparison->op, comparison->value,
                              plan.estimates.estimatedRows) /
                        N,
                    0.0, 1.0);
            }
            missProduct *= 1.0 - selectivity;
        }
        const double unionCost = std::max(N * (1.0 - missProduct), 1.0);

        UnionPlan unionPlan;
        unionPlan.unionProbes.reserve(indexable.size());
        std::ostringstream labels;
        for (const Predicate *pred : indexable) {
            auto probe = makeEqualityProbe(*pred);
            if (!labels.str().empty()) {
                labels << ", ";
            }
            labels << probeLabel(probe);
            unionPlan.unionProbes.push_back(std::move(probe));
        }
        plan.path = std::move(unionPlan);
        plan.estimates.estimatedCost = unionCost;
        plan.estimates.estimatedRows = rowsFromCost(unionCost, stats.rowCount());
        plan.estimates.explanation = "multi-index union on " + labels.str();
        plan.estimates.residual = buildOrTree(residualDisjuncts);
        if (plan.estimates.residual) {
            plan.estimates.notes.push_back(
                "residual OR complementary scan for non-indexable disjuncts");
        }
        return plan;
    }

    std::vector<const Predicate *> conjuncts;
    collectAndConjuncts(*query.where, conjuncts);

    std::size_t bestIndex = conjuncts.size();
    AccessPath bestPath = AccessPath::FullScan;
    double bestCost = plan.estimates.estimatedCost;

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
        if (isIndexableComparison(*conjuncts[i], stats, indexes, path, comparisonCost,
                                  plan.estimates.estimatedRows)) {
            consider(i, path, comparisonCost);
        }
        double inListCost = bestCost;
        if (isIndexableInList(*conjuncts[i], stats, indexes, inListCost,
                              plan.estimates.estimatedRows)) {
            consider(i, AccessPath::HashIndexInLookup, inListCost);
        }
    }

    std::vector<std::size_t> equalityIndexes;
    for (std::size_t i = 0; i < conjuncts.size(); ++i) {
        if (isEqualityIndexProbe(*conjuncts[i], indexes)) {
            equalityIndexes.push_back(i);
        }
    }

    bool choseIntersect = false;
    double intersectCost = bestCost;
    double intersectRows = 0.0;
    if (equalityIndexes.size() >= 2) {
        // Independence: N * Π(1/D_i). Cheaper than the best single index when combination is more
        // selective than any one conjunct alone.
        intersectRows =
            static_cast<double>(std::max<std::size_t>(plan.estimates.estimatedRows, 1));
        for (const auto index : equalityIndexes) {
            const double fanout =
                equalityFanout(*conjuncts[index], stats, indexes,
                               plan.estimates.estimatedRows);
            const double selectivity =
                fanout /
                static_cast<double>(std::max<std::size_t>(plan.estimates.estimatedRows, 1));
            intersectRows *= std::clamp(selectivity, 0.0, 1.0);
        }
        intersectCost = std::max(intersectRows, 1.0);
        if (intersectCost < bestCost) {
            choseIntersect = true;
        }
    }

    if (!choseIntersect && bestIndex == conjuncts.size()) {
        plan.estimates.residual = query.where;
        return plan;
    }

    if (choseIntersect) {
        IntersectPlan intersect;
        intersect.intersectProbes.reserve(equalityIndexes.size());
        plan.estimates.estimatedCost = intersectCost;
        plan.estimates.estimatedRows = rowsFromCost(intersectCost, stats.rowCount());
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
                intersect.intersectProbes.push_back(std::move(probe));
            } else {
                residualConjuncts.push_back(conjuncts[i]);
            }
        }
        plan.path = std::move(intersect);
        plan.estimates.explanation = "multi-index intersect on " + labels.str();
        plan.estimates.residual = buildAndTree(residualConjuncts);
        if (plan.estimates.residual) {
            plan.estimates.notes.push_back("residual filter applied after index lookup");
        }
        return plan;
    }

    const Predicate &chosen = *conjuncts[bestIndex];
    plan.estimates.estimatedCost = bestCost;

    if (bestPath == AccessPath::HashIndexLookup) {
        const auto &comparison = std::get<ComparisonPred>(chosen);
        plan.path = HashEqPlan{comparison.column, comparison.expression, comparison.value};
        plan.estimates.estimatedRows = rowsFromCost(bestCost, stats.rowCount());
        if (comparison.expression) {
            plan.estimates.explanation = "expression hash index equality lookup on (" +
                                         indexExpressionToString(*comparison.expression) + ")";
        } else {
            plan.estimates.explanation = "hash index equality lookup on " + comparison.column;
        }
    } else if (bestPath == AccessPath::OrderedIndexRange) {
        const auto &comparison = std::get<ComparisonPred>(chosen);
        plan.path = OrderedRangePlan{comparison.column, comparison.expression, comparison.op,
                                     comparison.value};
        plan.estimates.estimatedRows = rowsFromCost(bestCost, stats.rowCount());
        if (comparison.expression) {
            plan.estimates.explanation = "expression ordered index range lookup on (" +
                                         indexExpressionToString(*comparison.expression) + ")";
        } else {
            plan.estimates.explanation = "ordered index range lookup on " + comparison.column;
        }
    } else if (bestPath == AccessPath::HashIndexInLookup) {
        const auto &inList = std::get<InListPred>(chosen);
        plan.path = HashInPlan{inList.column, inList.expression, inList.inValues};
        plan.estimates.estimatedRows = rowsFromCost(bestCost, stats.rowCount());
        plan.estimates.explanation = "hash index IN lookup on " + inList.column + " (" +
                                     std::to_string(inList.inValues.size()) + " values)";
    }

    std::vector<const Predicate *> residualConjuncts;
    residualConjuncts.reserve(conjuncts.size() - 1);
    for (std::size_t i = 0; i < conjuncts.size(); ++i) {
        if (i != bestIndex) {
            residualConjuncts.push_back(conjuncts[i]);
        }
    }
    plan.estimates.residual = buildAndTree(residualConjuncts);
    if (plan.estimates.residual) {
        plan.estimates.notes.push_back("residual filter applied after index lookup");
    }
    return plan;
}

QueryPlan QueryPlanner::planSelect(const Select &query, const Table &table) const {
    return planSelect(query, static_cast<const RelationStats &>(table),
                      static_cast<const IndexCatalogView &>(table));
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
        const double fanout =
            averageRowsPerKey(rightRows, distinctOrOne(right, right, join.rightColumn));
        considerNested(true, static_cast<double>(std::max<std::size_t>(leftRows, 1)) * fanout,
                       right.name(), join.rightColumn);
    }
    if (leftIndexed) {
        const double fanout =
            averageRowsPerKey(leftRows, distinctOrOne(left, left, join.leftColumn));
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
        const double fanout =
            averageRowsPerKey(rightRows, distinctOrOne(right, right, join.rightColumn));
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
    out << plan.estimates.explanation;
    for (const auto &note : plan.estimates.notes) {
        out << "\n" << note;
    }
    if (plan.estimates.residual) {
        out << "\nresidual: yes";
    } else if (plan.accessPath() != AccessPath::FullScan) {
        out << "\nresidual: no";
    }
    out << "\nest_rows=" << plan.estimates.estimatedRows
        << " cost=" << plan.estimates.estimatedCost;
    return out.str();
}

std::string formatJoinPlanExplanation(const JoinPlan &plan) {
    std::ostringstream out;
    out << plan.explanation;
    out << "\nest_rows=" << plan.estimatedRows << " cost=" << plan.estimatedCost;
    return out.str();
}

} // namespace VertexDB
