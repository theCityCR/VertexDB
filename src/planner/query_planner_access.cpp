#include "query_planner_access.hpp"

#include "VertexDB/common/index_expression.hpp"
#include "VertexDB/common/string_pattern.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <utility>
#include <vector>

namespace VertexDB {
namespace planner_detail {

double unionSelectivity(const IndexBitmapNode &unionNode, const RelationStats &stats,
                        const IndexCatalogView &indexes, std::size_t estimatedRows) {
    const double N = static_cast<double>(std::max<std::size_t>(estimatedRows, 1));
    double missProduct = 1.0;
    for (const auto &child : unionNode.children) {
        if (child.kind != IndexBitmapNode::Kind::Probe) {
            continue;
        }
        const auto &probe = child.probe;
        double fanout = averageRowsPerKey(estimatedRows, 1);
        if (probe.expression) {
            fanout = averageRowsPerKey(estimatedRows, distinctOrOne(indexes, *probe.expression));
        } else {
            fanout = averageRowsPerKey(estimatedRows, distinctOrOne(stats, indexes, probe.column));
        }
        missProduct *= 1.0 - std::clamp(fanout / N, 0.0, 1.0);
    }
    return 1.0 - missProduct;
}

double nodeSelectivity(const IndexBitmapNode &node, const RelationStats &stats,
                       const IndexCatalogView &indexes, std::size_t estimatedRows) {
    const double N = static_cast<double>(std::max<std::size_t>(estimatedRows, 1));
    if (node.kind == IndexBitmapNode::Kind::Probe) {
        const auto &probe = node.probe;
        double fanout = averageRowsPerKey(estimatedRows, 1);
        if (probe.expression) {
            fanout = averageRowsPerKey(estimatedRows, distinctOrOne(indexes, *probe.expression));
        } else {
            fanout = averageRowsPerKey(estimatedRows, distinctOrOne(stats, indexes, probe.column));
        }
        return std::clamp(fanout / N, 0.0, 1.0);
    }
    if (node.kind == IndexBitmapNode::Kind::Union) {
        return unionSelectivity(node, stats, indexes, estimatedRows);
    }
    // Nested intersect under union (rare): product of child selectivities.
    double product = 1.0;
    for (const auto &child : node.children) {
        product *= nodeSelectivity(child, stats, indexes, estimatedRows);
    }
    return std::clamp(product, 0.0, 1.0);
}

BestConjunctChoice chooseBestConjunctPath(const std::vector<const Predicate *> &conjuncts,
                                          const RelationStats &stats,
                                          const IndexCatalogView &indexes, double fullScanCost,
                                          std::size_t estimatedRows) {
    BestConjunctChoice choice;
    choice.bestIndex = conjuncts.size();
    choice.bestPath = AccessPath::FullScan;
    choice.bestCost = fullScanCost;

    auto consider = [&](std::size_t i, AccessPath path, double cost,
                        std::optional<IntersectPlan> trigram = std::nullopt,
                        std::optional<UnionPlan> unionPlan = std::nullopt) {
        const bool betterCost = cost < choice.bestCost;
        const bool firstIndexAtParity = cost == choice.bestCost &&
                                        choice.bestPath == AccessPath::FullScan &&
                                        path != AccessPath::FullScan;
        const bool preferEquality =
            cost == choice.bestCost && path == AccessPath::HashEq &&
            choice.bestPath != AccessPath::HashEq && choice.bestPath != AccessPath::FullScan;
        if (betterCost || firstIndexAtParity || preferEquality) {
            choice.bestIndex = i;
            choice.bestPath = path;
            choice.bestCost = cost;
            choice.bestTrigramIntersect = std::move(trigram);
            choice.bestUnion = std::move(unionPlan);
        }
    };

    const double N = static_cast<double>(std::max<std::size_t>(estimatedRows, 1));
    for (std::size_t i = 0; i < conjuncts.size(); ++i) {
        AccessPath path = AccessPath::FullScan;
        double comparisonCost = choice.bestCost;
        if (isIndexableComparison(*conjuncts[i], stats, indexes, path, comparisonCost,
                                  estimatedRows)) {
            consider(i, path, comparisonCost);
        }
        double inListCost = choice.bestCost;
        if (isIndexableInList(*conjuncts[i], stats, indexes, inListCost, estimatedRows)) {
            consider(i, AccessPath::HashIn, inListCost);
        }
        AccessPath likePath = AccessPath::FullScan;
        double likeCost = choice.bestCost;
        std::optional<IntersectPlan> trigramIntersect;
        if (isIndexableLike(*conjuncts[i], indexes, likePath, likeCost, estimatedRows,
                            trigramIntersect)) {
            consider(i, likePath, likeCost, std::move(trigramIntersect));
        }
        if (auto unionNode = tryMakeFullyIndexableOrUnion(*conjuncts[i], indexes)) {
            const double sel = unionSelectivity(*unionNode, stats, indexes, estimatedRows);
            const double unionCost = std::max(N * sel, 1.0);
            UnionPlan unionPlan;
            unionPlan.children = std::move(unionNode->children);
            consider(i, AccessPath::Union, unionCost, std::nullopt, std::move(unionPlan));
        }
    }
    return choice;
}

void finalizeBestAccessPath(const std::vector<const Predicate *> &conjuncts,
                            const BestConjunctChoice &choice, const RelationStats &stats,
                            QueryPlan &plan) {
    const Predicate &chosen = *conjuncts[choice.bestIndex];
    plan.estimates.estimatedCost = choice.bestCost;

    if (choice.bestPath == AccessPath::HashEq) {
        const auto &comparison = std::get<ComparisonPred>(chosen);
        plan.path = HashEqPlan{comparison.column, comparison.expression, comparison.value, {}};
        plan.estimates.estimatedRows = rowsFromCost(choice.bestCost, stats.rowCount());
        if (comparison.expression) {
            plan.estimates.explanation = "expression hash index equality lookup on (" +
                                         indexExpressionToString(*comparison.expression) + ")";
        } else {
            plan.estimates.explanation = "hash index equality lookup on " + comparison.column;
        }
    } else if (choice.bestPath == AccessPath::OrderedRange) {
        const auto &comparison = std::get<ComparisonPred>(chosen);
        plan.path = OrderedRangePlan{comparison.column, comparison.expression, comparison.op,
                                     comparison.value};
        plan.estimates.estimatedRows = rowsFromCost(choice.bestCost, stats.rowCount());
        if (comparison.expression) {
            plan.estimates.explanation = "expression ordered index range lookup on (" +
                                         indexExpressionToString(*comparison.expression) + ")";
        } else {
            plan.estimates.explanation = "ordered index range lookup on " + comparison.column;
        }
    } else if (choice.bestPath == AccessPath::HashIn) {
        const auto &inList = std::get<InListPred>(chosen);
        plan.path = HashInPlan{inList.column, inList.expression, inList.inValues};
        plan.estimates.estimatedRows = rowsFromCost(choice.bestCost, stats.rowCount());
        if (inList.expression) {
            plan.estimates.explanation =
                "expression hash index IN lookup on (" +
                indexExpressionToString(*inList.expression) + ") (" +
                std::to_string(inList.inValues.size()) + " values)";
        } else {
            plan.estimates.explanation = "hash index IN lookup on " + inList.column + " (" +
                                         std::to_string(inList.inValues.size()) + " values)";
        }
    } else if (choice.bestPath == AccessPath::PrefixLike) {
        const auto &like = std::get<LikePred>(chosen);
        const auto prefix = likePrefixLiteral(like.pattern).value();
        plan.path = PrefixLikePlan{like.column, prefix};
        plan.estimates.estimatedRows = rowsFromCost(choice.bestCost, stats.rowCount());
        plan.estimates.explanation = "ordered index prefix LIKE on " + like.column;
    } else if (choice.bestPath == AccessPath::Intersect && choice.bestTrigramIntersect) {
        plan.path = *choice.bestTrigramIntersect;
        plan.estimates.estimatedRows = rowsFromCost(choice.bestCost, stats.rowCount());
        plan.estimates.explanation =
            "trigram intersect for LIKE on " + std::get<LikePred>(chosen).column;
    } else if (choice.bestPath == AccessPath::Union && choice.bestUnion) {
        plan.path = *choice.bestUnion;
        plan.estimates.estimatedRows = rowsFromCost(choice.bestCost, stats.rowCount());
        std::ostringstream labels;
        for (std::size_t i = 0; i < choice.bestUnion->children.size(); ++i) {
            if (i > 0) {
                labels << ", ";
            }
            labels << bitmapNodeLabel(choice.bestUnion->children[i]);
        }
        plan.estimates.explanation = "multi-index union on " + labels.str();
    }

    std::vector<const Predicate *> residualConjuncts;
    residualConjuncts.reserve(conjuncts.size());
    for (std::size_t i = 0; i < conjuncts.size(); ++i) {
        // Prefix/trigram LIKE probes are approximate; always keep the LIKE as a residual.
        if (i == choice.bestIndex && choice.bestPath != AccessPath::PrefixLike &&
            !(choice.bestPath == AccessPath::Intersect && choice.bestTrigramIntersect)) {
            continue;
        }
        if (i == choice.bestIndex &&
            (choice.bestPath == AccessPath::PrefixLike ||
             (choice.bestPath == AccessPath::Intersect && choice.bestTrigramIntersect))) {
            residualConjuncts.push_back(conjuncts[i]);
            continue;
        }
        if (i != choice.bestIndex) {
            residualConjuncts.push_back(conjuncts[i]);
        }
    }
    plan.estimates.residual = buildAndTree(residualConjuncts);
    if (plan.estimates.residual) {
        plan.estimates.notes.push_back("residual filter applied after index lookup");
    }
}

} // namespace planner_detail
} // namespace VertexDB
