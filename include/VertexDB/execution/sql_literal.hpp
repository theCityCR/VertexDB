#pragma once

// Format Value / Row for SQL literals and CLI display.
// Implementation: src/execution/sql_literal.cpp.

#include "VertexDB/common/value.hpp"
#include "VertexDB/parser/ast.hpp"
#include "VertexDB/storage/row.hpp"

#include <string>
#include <string_view>

namespace VertexDB {

[[nodiscard]] std::string sqlLiteral(const Value &value);
[[nodiscard]] std::string predicateLiteral(const Predicate &predicate);
[[nodiscard]] std::string createTableSql(const CreateTable &command);
[[nodiscard]] std::string insertSql(std::string_view table, const Row &row);
[[nodiscard]] std::string updateSql(const Update &command);
[[nodiscard]] std::string deleteSql(const Delete &command);
[[nodiscard]] std::string createIndexSql(const CreateIndex &command);

} // namespace VertexDB
