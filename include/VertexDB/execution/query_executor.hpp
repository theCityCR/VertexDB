#pragma once

#include "VertexDB/concurrency/lock_manager.hpp"
#include "VertexDB/execution/query_result.hpp"
#include "VertexDB/parser/ast.hpp"
#include "VertexDB/persistence/storage_manager.hpp"
#include "VertexDB/persistence/write_ahead_log.hpp"
#include "VertexDB/planner/query_planner.hpp"
#include "VertexDB/storage/database.hpp"
#include "VertexDB/transaction/transaction_manager.hpp"

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

  private:
    [[nodiscard]] QueryResult executeCreateDatabase(const CreateDatabase &command);
    [[nodiscard]] QueryResult executeCreateTable(const CreateTable &command);
    [[nodiscard]] QueryResult executeDropTable(const DropTable &command);
    [[nodiscard]] QueryResult executeRenameTable(const RenameTable &command);
    [[nodiscard]] QueryResult executeListTables();
    [[nodiscard]] QueryResult executeInsert(const Insert &command);
    [[nodiscard]] QueryResult executeSelect(const Select &command);
    [[nodiscard]] QueryResult executeUpdate(const Update &command);
    [[nodiscard]] QueryResult executeDelete(const Delete &command);
    [[nodiscard]] QueryResult executeCreateIndex(const CreateIndex &command);
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
    [[nodiscard]] std::vector<Row> collectRows(const Select &command, const Table &table) const;
    [[nodiscard]] QueryResult executeJoinSelect(const Select &command);
    [[nodiscard]] bool matches(const Row &row, const Table &table,
                               const Predicate &predicate) const;
    [[nodiscard]] std::shared_ptr<Table> requireTable(std::string_view tableName) const;
    [[nodiscard]] QueryResult executeUnlocked(const Query &query);
    [[nodiscard]] std::string bindPreparedSql(const ExecutePrepared &command) const;
    [[nodiscard]] std::optional<TransactionId> readVersion() const;
    [[nodiscard]] std::vector<Row> rowsSnapshotForRead(const Table &table) const;
    [[nodiscard]] std::vector<Row> rowsByIdForRead(const Table &table,
                                                   std::span<const RowId> rowIds) const;
    void recoverFromStorage();
    void recoverFromWal(bool loadedSnapshot);
    void appendWal(WalOperation operation, std::string payload);

    std::shared_ptr<Database> database_;
    std::shared_ptr<Database> transactionSnapshot_;
    StorageManager storageManager_;
    WriteAheadLog wal_;
    QueryPlanner planner_;
    TransactionManager transactionManager_;
    std::optional<TransactionId> activeTransaction_;
    std::unordered_map<std::string, std::string> preparedStatements_;
    bool replayingWal_{false};
    LockManager lockManager_;
};

} // namespace VertexDB
