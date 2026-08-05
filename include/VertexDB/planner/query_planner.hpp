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

class QueryPlanner {
  public:
    [[nodiscard]] QueryPlan planSelect(const Select &query, const Table &table) const;
};

[[nodiscard]] std::string formatPlanExplanation(const QueryPlan &plan);

} // namespace VertexDB
