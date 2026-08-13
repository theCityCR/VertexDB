#pragma once

// SELECT / join / EXPLAIN execution owned by SelectEngine.
// Implementation: select_engine.cpp (+ select_engine_scan.cpp, select_engine_join.cpp,
// select_engine_ssi.cpp, select_engine_bitmap.cpp).
// QueryExecutor remains the public façade; shared services live in ExecutionContext.
// Predicate matching (including correlated IN/EXISTS) is owned by SubqueryRuntime;
// scan/join call ctx_.subquery->matches directly.
// Peer contract: DmlEngine/CatalogEngine/SubqueryRuntime may call requireTable,
// collectVisibleEntries, collectRows, planPreparedSelect, executeJoinSelect,
// finalizeSelectResult. SelectEngine may call SubqueryRuntime::matches / prepare helpers
// only (no mutual recursion through execute/explain).

#include "VertexDB/execution/execution_context.hpp"
#include "VertexDB/execution/query_result.hpp"
#include "VertexDB/parser/ast.hpp"
#include "VertexDB/planner/query_planner.hpp"
#include "VertexDB/storage/table.hpp"

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace VertexDB {

struct RewriteResult;

// Filled only during EXPLAIN ANALYZE. candidates is set when a residual filter ran.
struct ExplainAnalyzeStats {
    std::optional<std::size_t> candidates;
    std::size_t actualRows{0};
    std::vector<std::size_t> joinActualRows;
};

class SelectEngine {
  public:
    explicit SelectEngine(ExecutionContext &ctx) noexcept;

    [[nodiscard]] QueryResult execute(const Select &command);
    [[nodiscard]] QueryResult explain(const ExplainQuery &command);
    [[nodiscard]] std::shared_ptr<Table>
    requireTable(std::string_view tableName,
                 const std::unordered_map<std::string, std::shared_ptr<Table>> &temps = {}) const;

    // Internal SELECT API used by SubqueryRuntime / DmlEngine / CatalogEngine
    // (via ExecutionContext::select).
    [[nodiscard]] QueryResult executeJoinSelect(
        const Select &command,
        const std::unordered_map<std::string, std::shared_ptr<Table>> &temps = {});
    [[nodiscard]] std::vector<Row> collectRows(const Select &command, const Table &table,
                                               const QueryPlan &plan,
                                               ExplainAnalyzeStats *stats = nullptr) const;
    // Same access-path visitor as collectRows, but keeps RowIds for UPDATE/DELETE.
    [[nodiscard]] std::vector<std::pair<RowId, Row>>
    collectVisibleEntries(const Select &command, const Table &table, const QueryPlan &plan,
                          ExplainAnalyzeStats *stats = nullptr) const;
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
        const std::unordered_map<std::string, std::shared_ptr<Table>> &temps = {},
        ExplainAnalyzeStats *stats = nullptr) const;
    [[nodiscard]] std::vector<Row> rowsSnapshotForRead(const Table &table) const;
    [[nodiscard]] std::vector<std::pair<RowId, Row>> visibleEntriesForRead(const Table &table) const;
    [[nodiscard]] std::vector<Row> rowsByIdForRead(const Table &table,
                                                   std::span<const RowId> rowIds) const;
    [[nodiscard]] std::vector<std::pair<RowId, Row>>
    entriesByIdForRead(const Table &table, std::span<const RowId> rowIds) const;

    ExecutionContext &ctx_;
};

} // namespace VertexDB
