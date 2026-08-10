#include "VertexDB/planner/query_planner.hpp"

#include "planner_detail.hpp"
#include "query_planner_access.hpp"

#include <optional>
#include <utility>
#include <vector>

namespace VertexDB {

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

    // Same-column equality OR → IN (top-level or nested under AND) so HashIn can win.
    std::optional<Predicate> rewrittenWhere;
    const Predicate *where = &*query.where;
    if (auto topLevelIn = tryRewriteSameColumnEqualityOrToIn(*query.where)) {
        rewrittenWhere = std::move(*topLevelIn);
        where = &*rewrittenWhere;
    }

    if (tryPlanTopLevelOrUnion(*where, stats, indexes, plan)) {
        return plan;
    }

    std::vector<const Predicate *> rawConjuncts;
    collectAndConjuncts(*where, rawConjuncts);

    std::vector<std::optional<Predicate>> conjunctRewrites(rawConjuncts.size());
    std::vector<const Predicate *> conjuncts(rawConjuncts.size());
    for (std::size_t i = 0; i < rawConjuncts.size(); ++i) {
        if (auto rewritten = tryRewriteSameColumnEqualityOrToIn(*rawConjuncts[i])) {
            conjunctRewrites[i] = std::move(*rewritten);
            conjuncts[i] = &*conjunctRewrites[i];
        } else {
            conjuncts[i] = rawConjuncts[i];
        }
    }

    const auto choice = chooseBestConjunctPath(conjuncts, stats, indexes, plan.estimates.estimatedCost,
                                               plan.estimates.estimatedRows);

    if (tryPlanAndIntersect(conjuncts, stats, indexes, choice.bestCost, plan)) {
        return plan;
    }

    if (choice.bestIndex == conjuncts.size()) {
        plan.estimates.residual = query.where;
        return plan;
    }

    finalizeBestAccessPath(conjuncts, choice, stats, plan);
    return plan;
}

} // namespace VertexDB
