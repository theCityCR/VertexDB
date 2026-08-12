#include "VertexDB/planner/query_planner.hpp"

#include <iomanip>
#include <sstream>
#include <utility>

namespace VertexDB {

std::string formatPlanExplanation(const QueryPlan &plan) {
    std::ostringstream out;
    out << plan.estimates.explanation;
    for (const auto &note : plan.estimates.notes) {
        out << "\n" << note;
    }
    if (plan.estimates.residual || plan.estimates.complementaryResidual) {
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

std::string appendExplainAnalyzeActuals(std::string planText, std::size_t actualRows,
                                        std::optional<std::size_t> candidates,
                                        std::optional<double> timeMs) {
    std::ostringstream out;
    out << std::move(planText);
    out << "\nactual_rows=" << actualRows;
    if (candidates) {
        out << "\ncandidates=" << *candidates;
    }
    if (timeMs) {
        out << "\nactual_time_ms=" << std::fixed << std::setprecision(3) << *timeMs;
    }
    return out.str();
}

} // namespace VertexDB
