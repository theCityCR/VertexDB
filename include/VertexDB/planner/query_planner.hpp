#pragma once

#include "VertexDB/parser/ast.hpp"
#include "VertexDB/storage/table.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace VertexDB {

enum class AccessPath : std::uint8_t {
    FullScan,
    HashIndexLookup,
    OrderedIndexRange,
    HashIndexInLookup,
};

enum class JoinAlgorithm : std::uint8_t {
    HashJoin,
    NestedLoopIndexProbe,
};

struct QueryPlan {
    AccessPath accessPath{AccessPath::FullScan};
    std::optional<Predicate> residual;
    std::string indexColumn;
    ComparisonOperator indexOp{ComparisonOperator::Equal};
    Value indexValue;
    std::vector<Value> indexValues;
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
    [[nodiscard]] QueryPlan planSelect(const Select &query, const Table &table) const;
    [[nodiscard]] JoinPlan planJoin(const Table &left, const Table &right,
                                    const JoinClause &join) const;
};

[[nodiscard]] std::string formatPlanExplanation(const QueryPlan &plan);
[[nodiscard]] std::string formatJoinPlanExplanation(const JoinPlan &plan);

} // namespace VertexDB
