#pragma once

// Hash aggregation and GROUP BY validation for SELECT.
// Implementation: select_aggregate.cpp.

#include "VertexDB/execution/query_result.hpp"
#include "VertexDB/parser/ast.hpp"
#include "VertexDB/storage/row.hpp"

#include <string>
#include <vector>

namespace VertexDB {

void validateAggregation(const Select &command);

[[nodiscard]] QueryResult aggregateRows(const Select &command,
                                        const std::vector<std::string> &sourceColumns,
                                        std::vector<Row> rows);

} // namespace VertexDB
