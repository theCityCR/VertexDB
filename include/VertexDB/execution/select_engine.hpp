#pragma once

// SELECT / join / EXPLAIN execution owned by SelectEngine.
// Implementation: select_engine.cpp (+ select_engine_scan.cpp, select_engine_join.cpp).
// QueryExecutor remains the public façade; shared services live in ExecutionContext.
// Predicate matching (including correlated IN/EXISTS) is owned by SubqueryRuntime;
// SelectEngine::matches forwards to keep scan/join call sites stable.

#include "VertexDB/execution/execution_context.hpp"
#include "VertexDB/execution/query_result.hpp"
#include "VertexDB/parser/ast.hpp"
#include "VertexDB/planner/query_planner.hpp"
#include "VertexDB/storage/table.hpp"

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace VertexDB {

struct RewriteResult;

class SelectEngine {
  public:
    explicit SelectEngine(ExecutionContext &ctx) noexcept;

    [[nodiscard]] QueryResult execute(const Select &command);
    [[nodiscard]] QueryResult explain(const ExplainQuery &command);
    [[nodiscard]] bool matches(const Row &row, const Table &table, const Predicate &predicate,
                               std::string_view scopeName = {}) const;
    [[nodiscard]] std::shared_ptr<Table>
    requireTable(std::string_view tableName,
                 const std::unordered_map<std::string, std::shared_ptr<Table>> &temps = {}) const;

    // Internal SELECT API used by SubqueryRuntime (via ExecutionContext::select).
    [[nodiscard]] QueryResult executeJoinSelect(
        const Select &command,
        const std::unordered_map<std::string, std::shared_ptr<Table>> &temps = {});
    [[nodiscard]] std::vector<Row> collectRows(const Select &command, const Table &table,
                                               const QueryPlan &plan) const;
    // Same access-path visitor as collectRows, but keeps RowIds for UPDATE/DELETE.
    [[nodiscard]] std::vector<std::pair<RowId, Row>>
    collectVisibleEntries(const Select &command, const Table &table, const QueryPlan &plan) const;
    [[nodiscard]] QueryPlan planPreparedSelect(const Select &command, const Table &table,
                                               const RewriteResult &rewrite) const;
    [[nodiscard]] QueryResult finalizeSelectResult(const Select &command,
                                                   std::vector<std::string> sourceColumns,
                                                   std::vector<Row> rows) const;

  private:
    [[nodiscard]] std::vector<std::size_t>
    resolveProjection(const Select &command, const Table &table,
                      std::vector<std::string> &columns) const;
    [[nodiscard]] std::vector<std::size_t>
    resolveProjectionFromNames(const Select &command, const std::vector<std::string> &sourceColumns,
                               std::vector<std::string> &projectedColumns) const;
    void collectJoinRows(
        const Select &command, std::vector<std::string> &joinedColumns, std::vector<Row> &joinedRows,
        const std::unordered_map<std::string, std::shared_ptr<Table>> &temps = {}) const;
    [[nodiscard]] std::vector<Row> rowsSnapshotForRead(const Table &table) const;
    [[nodiscard]] std::vector<std::pair<RowId, Row>> visibleEntriesForRead(const Table &table) const;
    [[nodiscard]] std::vector<Row> rowsByIdForRead(const Table &table,
                                                   std::span<const RowId> rowIds) const;
    [[nodiscard]] std::vector<std::pair<RowId, Row>>
    entriesByIdForRead(const Table &table, std::span<const RowId> rowIds) const;

    ExecutionContext &ctx_;
};

} // namespace VertexDB
