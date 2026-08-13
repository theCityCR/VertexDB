#pragma once

// Row-set helpers for UNION / INTERSECT / EXCEPT (distinct and ALL) and recursive UNION dedup.
// Projection / ORDER BY / LIMIT remain in select_helpers.hpp.

#include "VertexDB/execution/query_result.hpp"
#include "VertexDB/parser/ast.hpp"
#include "VertexDB/storage/row.hpp"

#include <string_view>
#include <vector>

namespace VertexDB {

struct RowHash {
    [[nodiscard]] std::size_t operator()(const Row &row) const;
};

[[nodiscard]] std::string_view setOpKindName(SetOpKind op) noexcept;

[[nodiscard]] QueryResult applySetOperation(SetOpKind op, QueryResult left, QueryResult right);

// Drop duplicate rows (first occurrence wins). Used by UNION and recursive UNION.
[[nodiscard]] std::vector<Row> deduplicateRows(std::vector<Row> rows);

} // namespace VertexDB
