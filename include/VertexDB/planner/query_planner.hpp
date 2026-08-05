#pragma once

#include "VertexDB/parser/ast.hpp"
#include "VertexDB/storage/table.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace VertexDB {

enum class AccessPath : std::uint8_t {
    FullScan,
    HashIndexLookup,
    OrderedIndexRange,
};

struct QueryPlan {
    AccessPath accessPath{AccessPath::FullScan};
    std::optional<Predicate> predicate;
    std::size_t estimatedRows{};
    double estimatedCost{};
    std::string explanation{"full table scan"};
};

class QueryPlanner {
  public:
    [[nodiscard]] QueryPlan planSelect(const Select &query, const Table &table) const;
};

} // namespace VertexDB
