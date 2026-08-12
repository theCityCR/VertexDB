#pragma once

// Row-level Predicate evaluation for comparison / AND / OR / LIKE / regex / literal IN.
// Implementation: predicate_eval.cpp.
// evalPredicate throws on IN-subquery and EXISTS arms — those need outer binding.
// Full entry point (including correlated IN/EXISTS): SubqueryRuntime::matches
// (SelectEngine::matches forwards there for scan/join call sites).

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
