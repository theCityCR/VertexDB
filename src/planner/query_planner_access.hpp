#pragma once

// Access-path strategy helpers used by query_planner_select.cpp (OR-union, AND-intersect,
// best-path finalize). Implementations:
//   query_planner_or_union.cpp      — tryPlanTopLevelOrUnion
//   query_planner_composite_eq.cpp  — tryPlanCompositeHashEq
//   query_planner_and_intersect.cpp — tryPlanAndIntersect
//   query_planner_access.cpp        — chooseBestConjunctPath, finalizeBestAccessPath,
//                                     shared selectivity helpers
// These are planner_detail free functions — not a separate public type.

#include "planner_detail.hpp"

#include <cstddef>
#include <optional>
#include <vector>

namespace VertexDB {
namespace planner_detail {

[[nodiscard]] double unionSelectivity(const IndexBitmapNode &unionNode, const RelationStats &stats,
                                      const IndexCatalogView &indexes, std::size_t estimatedRows);
[[nodiscard]] double nodeSelectivity(const IndexBitmapNode &node, const RelationStats &stats,
                                     const IndexCatalogView &indexes, std::size_t estimatedRows);

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

// When a composite (multi-column) index covers multi-equality AND cheaper than bestCost,
// fills plan and returns true. Prefers one composite HashEq probe over Intersect of singles.
[[nodiscard]] bool tryPlanCompositeHashEq(const std::vector<const Predicate *> &conjuncts,
                                          const RelationStats &stats,
                                          const IndexCatalogView &indexes, double bestCost,
                                          QueryPlan &plan);

// When multi-index AND intersect wins, fills plan and returns true.
[[nodiscard]] bool tryPlanAndIntersect(const std::vector<const Predicate *> &conjuncts,
                                       const RelationStats &stats, const IndexCatalogView &indexes,
                                       double bestCost, QueryPlan &plan);

void finalizeBestAccessPath(const std::vector<const Predicate *> &conjuncts,
                            const BestConjunctChoice &choice, const RelationStats &stats,
                            QueryPlan &plan);

} // namespace planner_detail
} // namespace VertexDB
