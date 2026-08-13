#pragma once

// CHECK constraint evaluation: reject only when the predicate is FALSE.
// NULL comparisons yield UNKNOWN (accepted), matching SQL CHECK semantics.
// Supports Comparison / AND / OR only (simple CHECK; no subqueries).

#include "VertexDB/parser/predicate.hpp"
#include "VertexDB/storage/row.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace VertexDB {

// Returns true when the row satisfies the CHECK (TRUE or UNKNOWN).
[[nodiscard]] bool evalCheckPredicate(
    const Predicate &predicate, const Row &row,
    const std::function<std::optional<std::size_t>(std::string_view)> &lookup);

// Rejects CHECK shapes outside simple column comparisons with AND/OR.
void assertSimpleCheckConstraint(const Predicate &predicate);

// Format a simple CHECK predicate for error messages / CREATE TABLE SQL.
[[nodiscard]] std::string checkConstraintLiteral(const Predicate &predicate);

} // namespace VertexDB
