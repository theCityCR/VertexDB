#pragma once

// Internal helpers for QueryPlanner TUs (predicate trees, costing, indexability).
// Implementations: query_planner_predicate.cpp (planner_detail free functions).
// Access-path finalize (OR-union / AND-intersect) is in query_planner_access.hpp —
// not a separate public type. Not part of the public include surface.

#include "VertexDB/planner/query_planner.hpp"
#include "VertexDB/storage/relation_stats.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace VertexDB {
namespace planner_detail {

void collectAndConjuncts(const Predicate &predicate, std::vector<const Predicate *> &out);
void collectOrDisjuncts(const Predicate &predicate, std::vector<const Predicate *> &out);

[[nodiscard]] std::optional<Predicate>
buildAndTree(const std::vector<const Predicate *> &conjuncts);
[[nodiscard]] std::optional<Predicate>
buildOrTree(const std::vector<const Predicate *> &disjuncts);

[[nodiscard]] double averageRowsPerKey(std::size_t rowCount, std::size_t distinctKeys);

[[nodiscard]] std::size_t distinctOrOne(const RelationStats &stats, const IndexCatalogView &indexes,
                                        std::string_view column);
[[nodiscard]] std::size_t distinctOrOne(const IndexCatalogView &indexes,
                                        const IndexExpression &expression);

[[nodiscard]] double rangeCost(const RelationStats &stats, std::string_view column,
                               ComparisonOperator op, const Value &value, std::size_t rowCount);
[[nodiscard]] double inCost(const RelationStats &stats, const IndexCatalogView &indexes,
                            std::string_view column, const std::vector<Value> &values,
                            std::size_t rowCount);

[[nodiscard]] bool isIndexableComparison(const Predicate &predicate, const RelationStats &stats,
                                         const IndexCatalogView &indexes, AccessPath &path,
                                         double &cost, std::size_t rowCount);
[[nodiscard]] bool isIndexableInList(const Predicate &predicate, const RelationStats &stats,
                                     const IndexCatalogView &indexes, double &cost,
                                     std::size_t rowCount);
// Fills path with PrefixLike or Intersect (trigram) when applicable; residual LIKE still required.
[[nodiscard]] bool isIndexableLike(const Predicate &predicate, const IndexCatalogView &indexes,
                                   AccessPath &path, double &cost, std::size_t rowCount,
                                   std::optional<IntersectPlan> &trigramIntersect);
[[nodiscard]] bool isEqualityIndexProbe(const Predicate &predicate,
                                        const IndexCatalogView &indexes);

// Rewrite (col=v1 OR col=v2 …) / same expression equalities into InListPred when every
// disjunct is a constant equality on the same column (and expression, if any).
[[nodiscard]] std::optional<Predicate>
tryRewriteSameColumnEqualityOrToIn(const Predicate &predicate);

[[nodiscard]] IndexEqualityProbe makeEqualityProbe(const Predicate &predicate);
[[nodiscard]] double equalityFanout(const Predicate &predicate, const RelationStats &stats,
                                    const IndexCatalogView &indexes, std::size_t rowCount);
[[nodiscard]] std::size_t rowsFromCost(double cost, std::size_t rowCount);
[[nodiscard]] std::string probeLabel(const IndexEqualityProbe &probe);
[[nodiscard]] std::string bitmapNodeLabel(const IndexBitmapNode &node);

// When every OR disjunct is an equality index probe, build a Union node (no residual arms).
[[nodiscard]] std::optional<IndexBitmapNode>
tryMakeFullyIndexableOrUnion(const Predicate &predicate, const IndexCatalogView &indexes);

} // namespace planner_detail
} // namespace VertexDB
