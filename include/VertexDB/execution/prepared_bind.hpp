#pragma once

// Prepared-statement parameter binding (? slots → values). See also select_helpers.hpp
// for projection/aggregation and query_executor.hpp for EXECUTE dispatch.

#include "VertexDB/common/value.hpp"
#include "VertexDB/parser/ast.hpp"

#include <vector>

namespace VertexDB {

[[nodiscard]] Query bindQueryParameters(const Query &query, const std::vector<Value> &parameters);

} // namespace VertexDB
