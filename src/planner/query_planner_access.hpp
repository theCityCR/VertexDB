#pragma once

// Access-path strategy helpers used by query_planner_select.cpp (OR-union, AND-intersect,
// best-path finalize). Implementations: query_planner_access.cpp.
// These are planner_detail free functions — not a separate public type.

#include "planner_detail.hpp"

#include <cstddef>
#include <optional>
#include <vector>

namespace VertexDB {
namespace planner_detail {

// When where is a top-level OrPred, fills plan and returns true. Otherwise returns false.
[[nodiscard]] bool tryPlanTopLevelOrUnion(const Predicate &where, const RelationStats &stats,
                                          const IndexCatalogView &indexes, QueryPlan &plan);

struct BestConjunctChoice {
    std::size_t bestIndex{0};
    AccessPath bestPath{AccessPath::FullScan};
    double bestCost{0.0};
    std::optional<IntersectPlan> bestTrigramIntersect;
    std::optional<UnionPlan> bestUnion;
};

[[nodiscard]] BestConjunctChoice chooseBestConjunctPath(
    const std::vector<const Predicate *> &conjuncts, const RelationStats &stats,
    const IndexCatalogView &indexes, double fullScanCost, std::size_t estimatedRows);

// When multi-index AND intersect wins, fills plan and returns true.
[[nodiscard]] bool tryPlanAndIntersect(const std::vector<const Predicate *> &conjuncts,
                                       const RelationStats &stats, const IndexCatalogView &indexes,
                                       double bestCost, QueryPlan &plan);

void finalizeBestAccessPath(const std::vector<const Predicate *> &conjuncts,
                            const BestConjunctChoice &choice, const RelationStats &stats,
                            QueryPlan &plan);

} // namespace planner_detail
} // namespace VertexDB
