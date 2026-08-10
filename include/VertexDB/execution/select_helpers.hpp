#pragma once

// SELECT projection, ORDER BY/LIMIT, and column resolve helpers.
// Scope rewrite: select_scope.hpp. Aggregation: select_aggregate.hpp.

#include "VertexDB/execution/query_result.hpp"
#include "VertexDB/storage/row.hpp"
#include "VertexDB/storage/table.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace VertexDB {

void sortRowsByColumn(std::vector<Row> &rows, std::size_t columnIndex, bool ascending);

[[nodiscard]] QueryResult projectWithLimit(std::vector<Row> rows,
                                           const std::vector<std::size_t> &projection,
                                           std::vector<std::string> columns,
                                           std::optional<std::size_t> limit,
                                           std::string message = "selected rows");

[[nodiscard]] std::optional<std::size_t> resolveResultColumn(std::span<const std::string> columns,
                                                             std::string_view requested);

[[nodiscard]] std::optional<std::size_t> resolveTableColumn(
    const Table &table, std::string_view tableName, std::string_view requested,
    std::optional<std::string_view> tableAlias = std::nullopt);

[[nodiscard]] QueryResult messageResult(bool success, std::string message);

} // namespace VertexDB
