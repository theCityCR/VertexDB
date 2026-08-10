#include "VertexDB/planner/query_planner.hpp"

#include "planner_detail.hpp"
#include "query_planner_access.hpp"

#include <utility>

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

    if (tryPlanTopLevelOrUnion(*query.where, stats, indexes, plan)) {
        return plan;
    }

    std::vector<const Predicate *> conjuncts;
    collectAndConjuncts(*query.where, conjuncts);

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
