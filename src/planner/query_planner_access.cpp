#include "query_planner_access.hpp"

#include "VertexDB/common/index_expression.hpp"
#include "VertexDB/common/string_pattern.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <utility>

namespace VertexDB {
namespace planner_detail {
namespace {

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

} // namespace

bool tryPlanTopLevelOrUnion(const Predicate &where, const RelationStats &stats,
                            const IndexCatalogView &indexes, QueryPlan &plan) {
    if (!std::holds_alternative<OrPred>(where)) {
        return false;
    }

    // Top-level OR: union equality index probes; non-indexable arms become a residual OR
    // complementary scan (partial OR) when the indexable subset is cheaper than a full scan.
    std::vector<const Predicate *> disjuncts;
    collectOrDisjuncts(where, disjuncts);

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
        plan.estimates.residual = where;
        plan.estimates.explanation = "full table scan (OR predicate)";
        return true;
    }

    // Path choice uses indexable arms only (independence: N * (1 - Π(1 - s_i))).
    const double N = static_cast<double>(std::max<std::size_t>(plan.estimates.estimatedRows, 1));
    double indexMissProduct = 1.0;
    for (const Predicate *pred : indexable) {
        const double fanout = equalityFanout(*pred, stats, indexes, plan.estimates.estimatedRows);
        const double selectivity = std::clamp(fanout / N, 0.0, 1.0);
        indexMissProduct *= 1.0 - selectivity;
    }
    const double indexUnionCost = std::max(N * (1.0 - indexMissProduct), 1.0);
    if (!(indexUnionCost < plan.estimates.estimatedCost)) {
        plan.estimates.residual = where;
        plan.estimates.explanation = "full table scan (OR predicate)";
        return true;
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
                   comparison != nullptr && !comparison->rhsColumn && !comparison->expression &&
                   (comparison->op == ComparisonOperator::Less ||
                    comparison->op == ComparisonOperator::Greater) &&
                   stats.columnHistogram(comparison->column)) {
            selectivity = std::clamp(rangeCost(stats, comparison->column, comparison->op,
                                               comparison->value, plan.estimates.estimatedRows) /
                                         N,
                                     0.0, 1.0);
        }
        missProduct *= 1.0 - selectivity;
    }
    const double unionCost = std::max(N * (1.0 - missProduct), 1.0);

    UnionPlan unionPlan;
    unionPlan.children.reserve(indexable.size());
    std::ostringstream labels;
    for (const Predicate *pred : indexable) {
        auto probe = makeEqualityProbe(*pred);
        if (!labels.str().empty()) {
            labels << ", ";
        }
        labels << probeLabel(probe);
        unionPlan.children.push_back(IndexBitmapNode::makeProbe(std::move(probe)));
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
    return true;
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

bool tryPlanAndIntersect(const std::vector<const Predicate *> &conjuncts,
                         const RelationStats &stats, const IndexCatalogView &indexes,
                         double bestCost, QueryPlan &plan) {
    // Collect equality probes and fully indexable nested OR unions as Intersect children.
    // Partial nested OR (any non-indexable arm) stays as an AND residual so filter semantics
    // remain correct (AND residual must not treat OR as complementary scan).
    std::vector<IndexBitmapNode> children;
    std::vector<const Predicate *> residualConjuncts;
    children.reserve(conjuncts.size());
    residualConjuncts.reserve(conjuncts.size());

    for (const Predicate *pred : conjuncts) {
        if (isEqualityIndexProbe(*pred, indexes)) {
            children.push_back(IndexBitmapNode::makeProbe(makeEqualityProbe(*pred)));
            continue;
        }
        if (auto unionNode = tryMakeFullyIndexableOrUnion(*pred, indexes)) {
            children.push_back(std::move(*unionNode));
            continue;
        }
        residualConjuncts.push_back(pred);
    }

    if (children.size() < 2) {
        return false;
    }

    // Independence: N * Π(sel_child). Union children use 1 - Π(1 - s_i).
    double intersectRows =
        static_cast<double>(std::max<std::size_t>(plan.estimates.estimatedRows, 1));
    for (const auto &child : children) {
        intersectRows *=
            nodeSelectivity(child, stats, indexes, plan.estimates.estimatedRows);
    }
    const double intersectCost = std::max(intersectRows, 1.0);
    if (!(intersectCost < bestCost)) {
        return false;
    }

    IntersectPlan intersect;
    intersect.children = std::move(children);
    plan.estimates.estimatedCost = intersectCost;
    plan.estimates.estimatedRows = rowsFromCost(intersectCost, stats.rowCount());

    std::ostringstream probeLabels;
    std::ostringstream unionLabels;
    bool hasProbe = false;
    bool hasNestedUnion = false;
    for (const auto &child : intersect.children) {
        if (child.kind == IndexBitmapNode::Kind::Union) {
            if (hasNestedUnion) {
                unionLabels << ", ";
            }
            hasNestedUnion = true;
            unionLabels << bitmapNodeLabel(child);
        } else {
            if (hasProbe) {
                probeLabels << ", ";
            }
            hasProbe = true;
            probeLabels << bitmapNodeLabel(child);
        }
    }
    std::ostringstream explanation;
    explanation << "multi-index intersect on ";
    if (hasProbe && hasNestedUnion) {
        explanation << probeLabels.str() << " with " << unionLabels.str();
    } else if (hasProbe) {
        explanation << probeLabels.str();
    } else {
        explanation << unionLabels.str();
    }
    plan.path = std::move(intersect);
    plan.estimates.explanation = explanation.str();
    if (hasNestedUnion) {
        plan.estimates.notes.push_back("composite Intersect∪Union for nested OR under AND");
    }
    plan.estimates.residual = buildAndTree(residualConjuncts);
    if (plan.estimates.residual) {
        plan.estimates.notes.push_back("residual filter applied after index lookup");
    }
    return true;
}

void finalizeBestAccessPath(const std::vector<const Predicate *> &conjuncts,
                            const BestConjunctChoice &choice, const RelationStats &stats,
                            QueryPlan &plan) {
    const Predicate &chosen = *conjuncts[choice.bestIndex];
    plan.estimates.estimatedCost = choice.bestCost;

    if (choice.bestPath == AccessPath::HashEq) {
        const auto &comparison = std::get<ComparisonPred>(chosen);
        plan.path = HashEqPlan{comparison.column, comparison.expression, comparison.value};
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
