#pragma once

#include "VertexDB/common/comparison_operator.hpp"
#include "VertexDB/common/value.hpp"
#include "VertexDB/parser/ast.hpp"
#include "VertexDB/storage/row.hpp"

#include <functional>
#include <optional>
#include <string_view>

namespace VertexDB {

using ColumnLookup = std::function<std::optional<std::size_t>(std::string_view)>;

[[nodiscard]] bool compareValues(const Value &left, ComparisonOperator op, const Value &right);
[[nodiscard]] bool evalPredicate(const Predicate &predicate, const Row &row,
                                 const ColumnLookup &lookup);

} // namespace VertexDB
