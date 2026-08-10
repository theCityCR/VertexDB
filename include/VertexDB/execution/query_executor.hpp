#pragma once

// SQL command façade: dispatch and public API only.
// SelectEngine, SubqueryRuntime, PreparedStatementCatalog, TxnSession, and
// RecoveryService own the execution subsystems composed by this façade.
// Shared SELECT/subquery services are exposed via ExecutionContext (no friends).

#include "VertexDB/concurrency/lock_manager.hpp"
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
#include "VertexDB/planner/rewriter.hpp"
#include "VertexDB/storage/database.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

namespace VertexDB {

class QueryExecutor {
  public:
    explicit QueryExecutor(std::filesystem::path storageRoot = "data");

    [[nodiscard]] QueryResult execute(const Query &query);
    [[nodiscard]] std::shared_ptr<Database> currentDatabase() const noexcept;
    [[nodiscard]] std::optional<Query> preparedAst(std::string_view name) const;

  private:
    [[nodiscard]] QueryResult executeCreateDatabase(const CreateDatabase &command);
    [[nodiscard]] QueryResult executeCreateTable(const CreateTable &command);
    [[nodiscard]] QueryResult executeDropTable(const DropTable &command);
    [[nodiscard]] QueryResult executeRenameTable(const RenameTable &command);
    [[nodiscard]] QueryResult executeListTables();
    [[nodiscard]] QueryResult executeInsert(const Insert &command);
    [[nodiscard]] QueryResult executeSelect(const Select &command);
    [[nodiscard]] QueryResult executeExplain(const ExplainQuery &command);
    [[nodiscard]] QueryResult executeUpdate(const Update &command);
    [[nodiscard]] QueryResult executeDelete(const Delete &command);
    [[nodiscard]] QueryResult executeCreateIndex(const CreateIndex &command);
    [[nodiscard]] QueryResult executeAnalyze(const Analyze &command);
    [[nodiscard]] QueryResult executeSaveDatabase();
    [[nodiscard]] QueryResult executeLoadDatabase(const LoadDatabase &command);
    [[nodiscard]] QueryResult executeBegin();
    [[nodiscard]] QueryResult executeCommit();
    [[nodiscard]] QueryResult executeRollback();
    [[nodiscard]] QueryResult executePrepare(const PrepareStatement &command);
    [[nodiscard]] QueryResult executePrepared(const ExecutePrepared &command);

    [[nodiscard]] bool matches(const Row &row, const Table &table,
                               const Predicate &predicate) const;
    [[nodiscard]] std::shared_ptr<Table>
    requireTable(std::string_view tableName,
                 const std::unordered_map<std::string, std::shared_ptr<Table>> &temps = {}) const;
    [[nodiscard]] QueryResult executeUnlocked(const Query &query);
    [[nodiscard]] ReadSnapshot readSnapshot() const;
    [[nodiscard]] TransactionId writeTransactionId();
    [[nodiscard]] bool transactionActive() const noexcept;
    [[nodiscard]] QueryResult rejectIfTransactionActive(std::string_view action) const;
    void appendWal(WalOperation operation, std::string payload);
    void appendPageImageRedo(Table &table, std::string tableName);
    void flushPendingWal();
    void clearPendingWal() noexcept;

    std::shared_ptr<Database> database_;
    StorageManager storageManager_;
    WriteAheadLog wal_;
    QueryPlanner planner_;
    TxnSession session_;
    RecoveryService recovery_;
    ExecutionContext ctx_;
    SelectEngine selectEngine_;
    SubqueryRuntime subqueryRuntime_;
    PreparedStatementCatalog prepared_;
    LockManager lockManager_;
};

} // namespace VertexDB
