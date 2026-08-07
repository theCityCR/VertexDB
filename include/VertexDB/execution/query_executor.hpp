#pragma once

// SQL command façade: dispatch, DDL/DML, transactions, prepared EXECUTE.
// SELECT/join live in query_executor_select.cpp; CTE/IN/EXISTS in
// query_executor_subquery.cpp; TxnSession and RecoveryService own transactional state.

#include "VertexDB/concurrency/lock_manager.hpp"
#include "VertexDB/execution/query_result.hpp"
#include "VertexDB/execution/recovery_service.hpp"
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
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

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
    [[nodiscard]] bool matches(const Row &row, const Table &table,
                               const Predicate &predicate) const;
    [[nodiscard]] Select prepareSelect(const Select &command, RewriteResult &rewrite) const;
    [[nodiscard]] Predicate materializePredicate(const Predicate &predicate) const;
    [[nodiscard]] std::vector<Value> evaluateSubqueryValues(const Select &subquery) const;
    [[nodiscard]] bool evaluateExists(const Select &subquery) const;
    [[nodiscard]] Select bindOuterReferences(const Select &subquery, const Row &outerRow,
                                             const Table &outerTable) const;
    [[nodiscard]] Predicate bindOuterReferences(const Predicate &predicate, const Row &outerRow,
                                                const Table &outerTable) const;
    [[nodiscard]] std::shared_ptr<Table>
    materializeCteTable(const std::string &name, const Select &body) const;
    [[nodiscard]] std::shared_ptr<Table>
    requireTable(std::string_view tableName,
                 const std::unordered_map<std::string, std::shared_ptr<Table>> &temps = {}) const;
    [[nodiscard]] QueryPlan planPreparedSelect(const Select &command, const Table &table,
                                               const RewriteResult &rewrite) const;
    [[nodiscard]] QueryResult executeUnlocked(const Query &query);
    [[nodiscard]] ReadSnapshot readSnapshot() const;
    [[nodiscard]] TransactionId writeTransactionId();
    [[nodiscard]] std::vector<Row> rowsSnapshotForRead(const Table &table) const;
    [[nodiscard]] std::vector<Row> rowsByIdForRead(const Table &table,
                                                   std::span<const RowId> rowIds) const;
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
    std::unordered_map<std::string, Query> preparedStatements_;
    LockManager lockManager_;
};

} // namespace VertexDB
