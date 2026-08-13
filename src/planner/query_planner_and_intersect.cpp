#include "query_planner_access.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <utility>
#include <vector>

namespace VertexDB {
namespace planner_detail {

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

} // namespace planner_detail
} // namespace VertexDB
