#pragma once

#include "VertexDB/execution/query_result.hpp"
#include "VertexDB/parser/ast.hpp"
#include "VertexDB/planner/query_planner.hpp"
#include "VertexDB/storage/table.hpp"

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace VertexDB {

class QueryExecutor;
class SubqueryRuntime;
struct RewriteResult;

class SelectEngine {
  public:
    explicit SelectEngine(QueryExecutor &owner) noexcept;

    [[nodiscard]] QueryResult execute(const Select &command);
    [[nodiscard]] QueryResult explain(const ExplainQuery &command);
    [[nodiscard]] bool matches(const Row &row, const Table &table,
                               const Predicate &predicate) const;
    [[nodiscard]] std::shared_ptr<Table>
    requireTable(std::string_view tableName,
                 const std::unordered_map<std::string, std::shared_ptr<Table>> &temps = {}) const;

  private:
    friend class SubqueryRuntime;

    [[nodiscard]] std::vector<std::size_t>
    resolveProjection(const Select &command, const Table &table,
                      std::vector<std::string> &columns) const;
    [[nodiscard]] std::vector<std::size_t>
    resolveProjectionFromNames(const Select &command, const std::vector<std::string> &sourceColumns,
                               std::vector<std::string> &projectedColumns) const;
    [[nodiscard]] std::vector<Row> collectRows(const Select &command, const Table &table,
                                               const QueryPlan &plan) const;
    [[nodiscard]] QueryResult executeJoinSelect(const Select &command);
    [[nodiscard]] QueryResult finalizeSelectResult(const Select &command,
                                                   std::vector<std::string> sourceColumns,
                                                   std::vector<Row> rows) const;
    void collectJoinRows(const Select &command, std::vector<std::string> &joinedColumns,
                         std::vector<Row> &joinedRows) const;
    [[nodiscard]] QueryPlan planPreparedSelect(const Select &command, const Table &table,
                                               const RewriteResult &rewrite) const;
    [[nodiscard]] std::vector<Row> rowsSnapshotForRead(const Table &table) const;
    [[nodiscard]] std::vector<Row> rowsByIdForRead(const Table &table,
                                                   std::span<const RowId> rowIds) const;

    QueryExecutor &owner_;
};

} // namespace VertexDB
