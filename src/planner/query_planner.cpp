#include "VertexDB/planner/query_planner.hpp"

#include "planner_detail.hpp"

#include "VertexDB/common/index_expression.hpp"
#include "VertexDB/storage/table.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <utility>

namespace VertexDB {

AccessPath QueryPlan::accessPath() const {
    return std::visit(
        [](const auto &path) {
            using T = std::decay_t<decltype(path)>;
            if constexpr (std::is_same_v<T, FullScanPlan>) {
                return AccessPath::FullScan;
            } else if constexpr (std::is_same_v<T, HashEqPlan>) {
                return AccessPath::HashEq;
            } else if constexpr (std::is_same_v<T, OrderedRangePlan>) {
                return AccessPath::OrderedRange;
            } else if constexpr (std::is_same_v<T, HashInPlan>) {
                return AccessPath::HashIn;
            } else if constexpr (std::is_same_v<T, IntersectPlan>) {
                return AccessPath::Intersect;
            } else {
                return AccessPath::Union;
            }
        },
        path);
}

QueryPlan QueryPlanner::planSelect(const Select &query, const RelationStats &stats,
                                   const IndexCatalogView &indexes) const {
    using namespace planner_detail;

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
            cost == bestCost && path == AccessPath::HashEq &&
            bestPath != AccessPath::HashEq && bestPath != AccessPath::FullScan;
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
            consider(i, AccessPath::HashIn, inListCost);
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

    if (bestPath == AccessPath::HashEq) {
        const auto &comparison = std::get<ComparisonPred>(chosen);
        plan.path = HashEqPlan{comparison.column, comparison.expression, comparison.value};
        plan.estimates.estimatedRows = rowsFromCost(bestCost, stats.rowCount());
        if (comparison.expression) {
            plan.estimates.explanation = "expression hash index equality lookup on (" +
                                         indexExpressionToString(*comparison.expression) + ")";
        } else {
            plan.estimates.explanation = "hash index equality lookup on " + comparison.column;
        }
    } else if (bestPath == AccessPath::OrderedRange) {
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
    } else if (bestPath == AccessPath::HashIn) {
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

} // namespace VertexDB
