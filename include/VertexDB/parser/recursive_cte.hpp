#pragma once

// Shared WITH RECURSIVE helpers: table-ref counting, SCC grouping for mutual recursion,
// and validation used by the parser / rewriter / executor.

#include "VertexDB/parser/ast.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace VertexDB {

[[nodiscard]] std::size_t countTableRefs(const Select &select, std::string_view tableName);

[[nodiscard]] bool selectReferencesTable(const Select &select, std::string_view tableName);

// Indexes of recursive CTEs in `ctes`, in declaration order.
[[nodiscard]] std::vector<std::size_t> recursiveCteIndexes(const std::vector<CteEntry> &ctes);

// Distinct recursive CTE names referenced from FROM/JOIN in `select` (names taken from `ctes`).
[[nodiscard]] std::vector<std::string> referencedRecursiveCteNames(const Select &select,
                                                                   const std::vector<CteEntry> &ctes);

// All recursive CTEs in dependency/SCC order for materialization (mutual groups stay together).
[[nodiscard]] std::vector<CteEntry> recursiveMaterializationOrder(const std::vector<CteEntry> &ctes);

// Recursive CTEs in the same strongly connected component as `name` (declaration order).
[[nodiscard]] std::vector<CteEntry> recursiveSccContaining(const std::vector<CteEntry> &ctes,
                                                           std::string_view name);

// Throws std::runtime_error on invalid recursive WITH shape.
void validateRecursiveCtes(const std::vector<CteEntry> &ctes);

} // namespace VertexDB
