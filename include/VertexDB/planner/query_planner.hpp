#pragma once

// Variant-based costed access-path selection. Each path stores only the probe
// fields valid for that physical operation. CTE/derived rewrite is in rewriter.hpp.
// AccessPath names match *Plan structs (HashEq ↔ HashEqPlan, …). Older docs may
// say HashIndexLookup / OrderedIndexRange / HashIndexInLookup / MultiIndex*.
// IntersectPlan / UnionPlan hold recursive IndexBitmapNode trees (composite
// Intersect∪Union). Sibling TUs: query_planner_select.cpp, query_planner_access.cpp
// (OR-union / AND-intersect / finalize), query_planner_predicate.cpp,
// query_planner_join.cpp, query_planner_format.cpp; shared helpers in
// src/planner/planner_detail.hpp.

#include "VertexDB/parser/ast.hpp"
#include "VertexDB/storage/relation_stats.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace VertexDB {

class Table;

enum class AccessPath : std::uint8_t {
    FullScan,
    HashEq,
    OrderedRange,
    HashIn,
    Intersect,
    Union,
    PrefixLike,
};

enum class JoinAlgorithm : std::uint8_t {
    HashJoin,
    NestedLoopIndexProbe,
};

struct IndexEqualityProbe {
    std::string column;
    std::optional<IndexExpression> expression;
    Value value;
};

// Recursive multi-index boolean tree: leaf probes, or nested Intersect/Union nodes.
// Top-level IntersectPlan / UnionPlan are the AccessPathPlan roots; children may nest
// the opposite connective (composite Intersect∪Union).
struct IndexBitmapNode {
    enum class Kind : std::uint8_t { Probe, Intersect, Union };

    Kind kind{Kind::Probe};
    IndexEqualityProbe probe{};
    std::vector<IndexBitmapNode> children{};

    [[nodiscard]] static IndexBitmapNode makeProbe(IndexEqualityProbe p) {
        IndexBitmapNode node;
        node.kind = Kind::Probe;
        node.probe = std::move(p);
        return node;
    }

    [[nodiscard]] static IndexBitmapNode makeIntersect(std::vector<IndexBitmapNode> kids) {
        IndexBitmapNode node;
        node.kind = Kind::Intersect;
        node.children = std::move(kids);
        return node;
    }

    [[nodiscard]] static IndexBitmapNode makeUnion(std::vector<IndexBitmapNode> kids) {
        IndexBitmapNode node;
        node.kind = Kind::Union;
        node.children = std::move(kids);
        return node;
    }
};

struct PlanEstimates {
    std::size_t estimatedRows{};
    double estimatedCost{};
    std::string explanation{"full table scan"};
    std::vector<std::string> notes;
    std::optional<Predicate> residual;
};

struct FullScanPlan {};

struct HashEqPlan {
    std::string indexColumn;
    std::optional<IndexExpression> indexExpression;
    Value indexValue;
};

struct OrderedRangePlan {
    std::string indexColumn;
    std::optional<IndexExpression> indexExpression;
    ComparisonOperator indexOp{};
    Value indexValue;
};

struct HashInPlan {
    std::string indexColumn;
    std::optional<IndexExpression> indexExpression;
    std::vector<Value> indexValues;
};

struct IntersectPlan {
    std::vector<IndexBitmapNode> children;
};

struct UnionPlan {
    std::vector<IndexBitmapNode> children;
};

struct PrefixLikePlan {
    std::string indexColumn;
    std::string prefix;
};

using AccessPathPlan =
    std::variant<FullScanPlan, HashEqPlan, OrderedRangePlan, HashInPlan, IntersectPlan, UnionPlan,
                 PrefixLikePlan>;

struct QueryPlan {
    AccessPathPlan path{FullScanPlan{}};
    PlanEstimates estimates{};

    [[nodiscard]] AccessPath accessPath() const;
    [[nodiscard]] std::optional<Predicate> &residual() { return estimates.residual; }
    [[nodiscard]] const std::optional<Predicate> &residual() const { return estimates.residual; }
};

struct JoinPlan {
    JoinAlgorithm algorithm{JoinAlgorithm::HashJoin};
    // When NestedLoopIndexProbe: scan the outer side and probe the inner via hash index.
    bool outerIsLeft{true};
    std::string probeTable;
    std::string probeColumn;
    std::size_t estimatedRows{};
    double estimatedCost{};
    std::string explanation{"hash join"};
};

class QueryPlanner {
  public:
    [[nodiscard]] QueryPlan planSelect(const Select &query, const RelationStats &stats,
                                       const IndexCatalogView &indexes) const;
    [[nodiscard]] QueryPlan planSelect(const Select &query, const Table &table) const;
    [[nodiscard]] JoinPlan planJoin(const Table &left, const Table &right,
                                    const JoinClause &join) const;
    // Left side is an intermediate row set (no indexes); only hash join or right-side index probe.
    [[nodiscard]] JoinPlan planJoinAgainstRows(std::size_t leftRows, const Table &right,
                                               const JoinClause &join) const;
};

[[nodiscard]] std::string formatPlanExplanation(const QueryPlan &plan);
[[nodiscard]] std::string formatJoinPlanExplanation(const JoinPlan &plan);

// Append EXPLAIN ANALYZE actuals; omit unset optional fields.
[[nodiscard]] std::string appendExplainAnalyzeActuals(
    std::string planText, std::size_t actualRows, std::optional<std::size_t> candidates = std::nullopt,
    std::optional<double> timeMs = std::nullopt);

} // namespace VertexDB
