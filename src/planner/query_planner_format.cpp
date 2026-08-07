#include "VertexDB/planner/query_planner.hpp"

#include <sstream>

namespace VertexDB {

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
