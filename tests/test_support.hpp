#pragma once

// Shared GoogleTest helpers for themed VertexDB suites.
// Temp executor roots and the common Employees seed live here so suites stay theme-focused.

#include "VertexDB/execution/query_executor.hpp"
#include "VertexDB/parser/parser.hpp"

#include <filesystem>
#include <string>
#include <string_view>

namespace VertexDB {

[[nodiscard]] std::filesystem::path makeTempRoot(std::string_view prefix, std::string_view suffix);

[[nodiscard]] QueryExecutor makeTempExecutor(std::string_view prefix, std::string_view suffix);

// Creates database `company`, table `Employees(id, name, salary)`, three rows, optional indexes.
void seedEmployees(QueryExecutor &executor, Parser &parser, bool indexId = true,
                   bool indexSalary = false);

} // namespace VertexDB
