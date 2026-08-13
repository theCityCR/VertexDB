#include "query_planner_access.hpp"

#include "VertexDB/common/index_expression.hpp"
#include "VertexDB/common/string_pattern.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

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
        missProduct *= 1.0 - residualDisjunctSelectivity(*pred, stats, indexes,
                                                         plan.estimates.estimatedRows);
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

bool tryPlanCompositeHashEq(const std::vector<const Predicate *> &conjuncts,
                            const RelationStats &stats, const IndexCatalogView &indexes,
                            double bestCost, QueryPlan &plan) {
    // Map simple column equality conjuncts (no expression / join rhs) to their predicates.
    struct EqLit {
        const Predicate *predicate{nullptr};
        Value value;
    };
    std::unordered_map<std::string, EqLit> equalityByColumn;
    for (const Predicate *pred : conjuncts) {
        const auto *comparison = std::get_if<ComparisonPred>(pred);
        if (comparison == nullptr || comparison->rhsColumn || comparison->expression ||
            comparison->op != ComparisonOperator::Equal) {
            continue;
        }
        // First conjunct wins if the same column appears twice (residual keeps the rest).
        equalityByColumn.emplace(comparison->column, EqLit{pred, comparison->value});
    }
    if (equalityByColumn.size() < 2) {
        return false;
    }

    const auto composites = indexes.compositeIndexColumnLists();
    if (composites.empty()) {
        return false;
    }

    struct Candidate {
        std::vector<std::string> columns;
        std::vector<Value> parts;
        std::vector<const Predicate *> absorbed;
        double cost{0.0};
    };
    std::optional<Candidate> best;

    for (const auto &columns : composites) {
        if (columns.size() < 2 || !indexes.hasIndex(columns)) {
            continue;
        }
        Candidate candidate;
        candidate.columns = columns;
        candidate.parts.reserve(columns.size());
        candidate.absorbed.reserve(columns.size());
        bool covered = true;
        for (const auto &column : columns) {
            const auto it = equalityByColumn.find(column);
            if (it == equalityByColumn.end()) {
                covered = false;
                break;
            }
            candidate.parts.push_back(it->second.value);
            candidate.absorbed.push_back(it->second.predicate);
        }
        if (!covered) {
            continue;
        }

        const std::size_t N = std::max<std::size_t>(stats.rowCount(), 1);
        std::size_t distinct = indexes.indexDistinctCount(columns).value_or(0);
        if (distinct == 0) {
            // Fall back to independence over per-column distincts when composite stats are cold.
            distinct = 1;
            for (const auto &column : columns) {
                const std::size_t part = distinctOrOne(stats, indexes, column);
                if (distinct > N / std::max<std::size_t>(part, 1)) {
                    distinct = N;
                    break;
                }
                distinct *= part;
            }
            distinct = std::min(distinct, N);
        }
        candidate.cost = averageRowsPerKey(N, distinct);
        // Prefer composite HashEq over FullScan / equal-cost single-conjunct paths (`<=`),
        // so multi-equality AND can use the composite even when fanout ties a scan.
        if (candidate.cost > bestCost) {
            continue;
        }
        if (!best || candidate.cost < best->cost ||
            (candidate.cost == best->cost && candidate.columns.size() > best->columns.size())) {
            best = std::move(candidate);
        }
    }

    if (!best) {
        return false;
    }

    HashEqPlan hashEq;
    hashEq.indexColumn = best->columns.front();
    hashEq.indexColumns = best->columns;
    hashEq.indexValue = Value::composite(std::move(best->parts));
    plan.path = std::move(hashEq);
    plan.estimates.estimatedCost = best->cost;
    plan.estimates.estimatedRows = rowsFromCost(best->cost, stats.rowCount());

    std::ostringstream labels;
    for (std::size_t i = 0; i < best->columns.size(); ++i) {
        if (i > 0) {
            labels << ", ";
        }
        labels << best->columns[i];
    }
    plan.estimates.explanation = "hash index equality lookup on " + labels.str();
    plan.estimates.notes.push_back("composite index equality for multi-column AND");

    std::vector<const Predicate *> residualConjuncts;
    residualConjuncts.reserve(conjuncts.size());
    for (const Predicate *pred : conjuncts) {
        const bool absorbed =
            std::any_of(best->absorbed.begin(), best->absorbed.end(),
                        [pred](const Predicate *taken) { return taken == pred; });
        if (!absorbed) {
            residualConjuncts.push_back(pred);
        }
    }
    plan.estimates.residual = buildAndTree(residualConjuncts);
    if (plan.estimates.residual) {
        plan.estimates.notes.push_back("residual filter applied after index lookup");
    }
    return true;
}

bool tryPlanAndIntersect(const std::vector<const Predicate *> &conjuncts,
                         const RelationStats &stats, const IndexCatalogView &indexes,
                         double bestCost, QueryPlan &plan) {
    // Collect equality probes and nested OR unions (fully or partially indexable) as
    // Intersect children. Partial nested OR keeps indexable arms in the Union child and
    // schedules non-indexable arms as a complementary residual under the outer AND.
    std::vector<IndexBitmapNode> children;
    std::vector<const Predicate *> residualConjuncts;
    std::vector<const Predicate *> equalityConjuncts;
    struct AbsorbedOr {
        const Predicate *original{};
        std::vector<const Predicate *> residualDisjuncts;
    };
    std::vector<AbsorbedOr> absorbedOrs;
    children.reserve(conjuncts.size());
    residualConjuncts.reserve(conjuncts.size());
    equalityConjuncts.reserve(conjuncts.size());
    absorbedOrs.reserve(conjuncts.size());

    for (const Predicate *pred : conjuncts) {
        if (isEqualityIndexProbe(*pred, indexes)) {
            children.push_back(IndexBitmapNode::makeProbe(makeEqualityProbe(*pred)));
            equalityConjuncts.push_back(pred);
            continue;
        }
        if (auto split = tryMakeOrUnionSplit(*pred, indexes)) {
            children.push_back(std::move(split->unionNode));
            absorbedOrs.push_back(AbsorbedOr{pred, std::move(split->residualDisjuncts)});
            continue;
        }
        residualConjuncts.push_back(pred);
    }

    if (children.size() < 2) {
        return false;
    }

    // Independence: N * Π(sel_child). Union children use 1 - Π(1 - s_i) over indexable arms.
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

    const bool hasPartialNestedOr = std::any_of(
        absorbedOrs.begin(), absorbedOrs.end(),
        [](const AbsorbedOr &absorbed) { return !absorbed.residualDisjuncts.empty(); });

    IntersectPlan intersect;
    intersect.children = std::move(children);
    // Path choice used indexable arms only; fold complementary arms into EXPLAIN estimates.
    double explainCost = intersectCost;
    if (hasPartialNestedOr) {
        const double tableN =
            static_cast<double>(std::max<std::size_t>(stats.rowCount(), 1));
        const double mainSel = std::clamp(intersectCost / tableN, 0.0, 1.0);
        double complementaryMiss = 1.0;
        for (const Predicate *eq : equalityConjuncts) {
            complementaryMiss *=
                1.0 - std::clamp(equalityFanout(*eq, stats, indexes, stats.rowCount()) / tableN,
                                 0.0, 1.0);
        }
        for (const auto &absorbed : absorbedOrs) {
            if (absorbed.residualDisjuncts.empty()) {
                if (auto full = tryMakeFullyIndexableOrUnion(*absorbed.original, indexes)) {
                    complementaryMiss *=
                        1.0 - nodeSelectivity(*full, stats, indexes, stats.rowCount());
                }
            } else {
                double orMiss = 1.0;
                for (const Predicate *arm : absorbed.residualDisjuncts) {
                    orMiss *= 1.0 - residualDisjunctSelectivity(*arm, stats, indexes,
                                                               stats.rowCount());
                }
                complementaryMiss *= orMiss;
            }
        }
        for (const Predicate *residual : residualConjuncts) {
            complementaryMiss *=
                1.0 - residualDisjunctSelectivity(*residual, stats, indexes, stats.rowCount());
        }
        const double complementarySel = 1.0 - complementaryMiss;
        const double combinedSel =
            std::clamp(mainSel + complementarySel - mainSel * complementarySel, 0.0, 1.0);
        explainCost = std::max(tableN * combinedSel, 1.0);
    }
    plan.estimates.estimatedCost = explainCost;
    plan.estimates.estimatedRows = rowsFromCost(explainCost, stats.rowCount());

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
    if (hasPartialNestedOr) {
        // Complementary rows must satisfy outer AND constraints: equality conjuncts and
        // residual AND filters in full, fully indexable nested ORs in full, and only the
        // non-indexable arms of each partial nested OR.
        std::optional<Predicate> complementary;
        auto appendAnd = [&](Predicate part) {
            if (!complementary) {
                complementary = std::move(part);
            } else {
                complementary = makeAnd(std::move(*complementary), std::move(part));
            }
        };
        for (const Predicate *eq : equalityConjuncts) {
            appendAnd(*eq);
        }
        for (const auto &absorbed : absorbedOrs) {
            if (absorbed.residualDisjuncts.empty()) {
                appendAnd(*absorbed.original);
            } else if (auto orTree = buildOrTree(absorbed.residualDisjuncts)) {
                appendAnd(std::move(*orTree));
            }
        }
        for (const Predicate *residual : residualConjuncts) {
            appendAnd(*residual);
        }
        plan.estimates.complementaryResidual = std::move(complementary);
        plan.estimates.notes.push_back(
            "partial nested OR complementary scan under AND for non-indexable disjuncts");
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
