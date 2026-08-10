#include "VertexDB/planner/query_planner.hpp"

#include "VertexDB/storage/table.hpp"

namespace VertexDB {

AccessPath QueryPlan::accessPath() const {
    return std::visit(
        [](const auto &path) {
            using T = std::decay_t<decltype(path)>;
            if constexpr (std::is_same_v<T, FullScanPlan>) {
                return AccessPath::FullScan;
            } else if constexpr (std::is_same_v<T, HashEqPlan>) {
                return AccessPath::HashEq;
            } else if constexpr (std::is_same_v<T, OrderedRangePlan>) {
                return AccessPath::OrderedRange;
            } else if constexpr (std::is_same_v<T, HashInPlan>) {
                return AccessPath::HashIn;
            } else if constexpr (std::is_same_v<T, IntersectPlan>) {
                return AccessPath::Intersect;
            } else if constexpr (std::is_same_v<T, PrefixLikePlan>) {
                return AccessPath::PrefixLike;
            } else {
                return AccessPath::Union;
            }
        },
        path);
}

QueryPlan QueryPlanner::planSelect(const Select &query, const Table &table) const {
    return planSelect(query, static_cast<const RelationStats &>(table),
                      static_cast<const IndexCatalogView &>(table));
}

} // namespace VertexDB
