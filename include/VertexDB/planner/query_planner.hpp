#pragma once

// Variant-based costed access-path selection. Each path stores only the probe
// fields valid for that physical operation. CTE/derived rewrite is in rewriter.hpp.

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
    HashIndexLookup,
    OrderedIndexRange,
    HashIndexInLookup,
    MultiIndexIntersect,
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
    std::vector<IndexEqualityProbe> intersectProbes;
};

using AccessPathPlan =
    std::variant<FullScanPlan, HashEqPlan, OrderedRangePlan, HashInPlan, IntersectPlan>;

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

} // namespace VertexDB
