#pragma once

// Internal helpers shared by select_engine_scan / ssi / bitmap TUs.
// Not a public API — keep declarations out of include/VertexDB.

#include "VertexDB/parser/ast.hpp"
#include "VertexDB/planner/query_planner.hpp"
#include "VertexDB/storage/table.hpp"
#include "VertexDB/transaction/transaction_manager.hpp"

#include <vector>

namespace VertexDB {
namespace select_scan_detail {

void recordSsiScanPredicates(TransactionManager &txns, TransactionId id, const Table &table,
                             const Select &command, const QueryPlan &plan);

[[nodiscard]] std::vector<RowId> evalIntersectPlan(const IntersectPlan &path, const Table &table);
[[nodiscard]] std::vector<RowId> evalUnionPlan(const UnionPlan &path, const Table &table);

} // namespace select_scan_detail
} // namespace VertexDB
