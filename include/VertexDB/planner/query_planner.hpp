#pragma once

// Cost-based access-path and join selection. CTE/derived rewrite is in rewriter.hpp.

#include "VertexDB/parser/ast.hpp"
#include "VertexDB/storage/relation_stats.hpp"

#include <cstdint>
#include <optional>
#include <string>
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

struct QueryPlan {
    AccessPath accessPath{AccessPath::FullScan};
    std::optional<Predicate> residual;
    std::string indexColumn;
    std::optional<IndexExpression> indexExpression;
    ComparisonOperator indexOp{ComparisonOperator::Equal};
    Value indexValue;
    std::vector<Value> indexValues;
    std::vector<IndexEqualityProbe> intersectProbes;
    std::size_t estimatedRows{};
    double estimatedCost{};
    std::string explanation{"full table scan"};
    std::vector<std::string> notes;
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
