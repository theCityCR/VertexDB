#pragma once

// Catalog DDL, ANALYZE, and SAVE/LOAD orchestration.
// Implementation: catalog_engine.cpp. QueryExecutor remains the public façade.

#include "VertexDB/execution/execution_context.hpp"
#include "VertexDB/execution/query_result.hpp"
#include "VertexDB/execution/recovery_service.hpp"
#include "VertexDB/parser/ast.hpp"
#include "VertexDB/persistence/storage_manager.hpp"
#include "VertexDB/persistence/write_ahead_log.hpp"

namespace VertexDB {

class CatalogEngine {
  public:
    CatalogEngine(ExecutionContext &ctx, RecoveryService &recovery, StorageManager &storage,
                  WriteAheadLog &wal) noexcept;

    [[nodiscard]] QueryResult executeCreateDatabase(const CreateDatabase &command);
    [[nodiscard]] QueryResult executeCreateTable(const CreateTable &command);
    [[nodiscard]] QueryResult executeDropTable(const DropTable &command);
    [[nodiscard]] QueryResult executeRenameTable(const RenameTable &command);
    [[nodiscard]] QueryResult executeListTables();
    [[nodiscard]] QueryResult executeCreateIndex(const CreateIndex &command);
    [[nodiscard]] QueryResult executeAnalyze(const Analyze &command);
    [[nodiscard]] QueryResult executeSaveDatabase();
    [[nodiscard]] QueryResult executeLoadDatabase(const LoadDatabase &command);

  private:
    ExecutionContext &ctx_;
    RecoveryService &recovery_;
    StorageManager &storage_;
    WriteAheadLog &wal_;
};

} // namespace VertexDB
