#include "query_planner_access.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <utility>
#include <vector>

namespace VertexDB {
namespace planner_detail {

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

} // namespace planner_detail
} // namespace VertexDB
