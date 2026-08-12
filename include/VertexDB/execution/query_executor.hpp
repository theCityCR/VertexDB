#pragma once

// SQL command façade: dispatch and public API only.
// CatalogEngine, SelectEngine, SubqueryRuntime, DmlEngine, PreparedStatementCatalog,
// TxnSession, and RecoveryService own the execution subsystems composed by this façade.
// Shared SELECT/subquery/DML/catalog services are exposed via ExecutionContext (no friends).

#include "VertexDB/concurrency/lock_manager.hpp"
#include "VertexDB/execution/catalog_engine.hpp"
#include "VertexDB/execution/dml_engine.hpp"
#include "VertexDB/execution/execution_context.hpp"
#include "VertexDB/execution/prepared_statement_catalog.hpp"
#include "VertexDB/execution/query_result.hpp"
#include "VertexDB/execution/recovery_service.hpp"
#include "VertexDB/execution/select_engine.hpp"
#include "VertexDB/execution/subquery_runtime.hpp"
#include "VertexDB/execution/txn_session.hpp"
#include "VertexDB/parser/ast.hpp"
#include "VertexDB/persistence/storage_manager.hpp"
#include "VertexDB/persistence/write_ahead_log.hpp"
#include "VertexDB/planner/query_planner.hpp"
#include "VertexDB/storage/database.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace VertexDB {

class QueryExecutor {
  public:
    explicit QueryExecutor(std::filesystem::path storageRoot = "data");

    [[nodiscard]] QueryResult execute(const Query &query);
    [[nodiscard]] std::shared_ptr<Database> currentDatabase() const noexcept;
    [[nodiscard]] std::optional<Query> preparedAst(std::string_view name) const;

  private:
    [[nodiscard]] QueryResult executeInsert(const Insert &command);
    [[nodiscard]] QueryResult executeSelect(const Select &command);
    [[nodiscard]] QueryResult executeExplain(const ExplainQuery &command);
    [[nodiscard]] QueryResult executeUpdate(const Update &command);
    [[nodiscard]] QueryResult executeDelete(const Delete &command);
    [[nodiscard]] QueryResult executeBegin();
    [[nodiscard]] QueryResult executeCommit();
    [[nodiscard]] QueryResult executeRollback();
    [[nodiscard]] QueryResult executePrepare(const PrepareStatement &command);
    [[nodiscard]] QueryResult executePrepared(const ExecutePrepared &command);

    [[nodiscard]] QueryResult executeUnlocked(const Query &query);

    std::shared_ptr<Database> database_;
    StorageManager storageManager_;
    WriteAheadLog wal_;
    QueryPlanner planner_;
    TxnSession session_;
    RecoveryService recovery_;
    ExecutionContext ctx_;
    SelectEngine selectEngine_;
    SubqueryRuntime subqueryRuntime_;
    DmlEngine dmlEngine_;
    CatalogEngine catalogEngine_;
    PreparedStatementCatalog prepared_;
    LockManager lockManager_;
};

} // namespace VertexDB
