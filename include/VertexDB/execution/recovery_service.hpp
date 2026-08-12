#pragma once

// WAL and undo recovery service for the QueryExecutor façade.
// Owns WAL replay policy, undo apply, and deferred flush; codecs stay in persistence/.
// Implementation: recovery_service.cpp.

#include "VertexDB/parser/ast.hpp"
#include "VertexDB/persistence/page_image_redo.hpp"
#include "VertexDB/persistence/physical_redo.hpp"
#include "VertexDB/persistence/storage_manager.hpp"
#include "VertexDB/persistence/write_ahead_log.hpp"
#include "VertexDB/storage/database.hpp"
#include "VertexDB/transaction/undo_log.hpp"

#include <functional>
#include <memory>
#include <string>

namespace VertexDB {

class TxnSession;

class RecoveryService {
  public:
    using ReplayQuery = std::function<void(const Query &)>;

    RecoveryService(StorageManager &storage, WriteAheadLog &wal, TxnSession &session,
                    std::shared_ptr<Database> &database, ReplayQuery replayQuery);

    void recoverFromStorage();
    void recoverFromWal(bool loadedSnapshot);
    void applyUndoRecord(const UndoRecord &record);
    void applyPhysicalRedo(const PhysicalRedoRecord &redo);
    void applyPageImageRedo(const PageImageRedoRecord &redo);
    void appendWal(WalOperation operation, std::string payload);
    void appendPageImageRedo(Table &table, std::string tableName);
    void flushPendingWal();
    void clearPendingWal() noexcept;

  private:
    [[nodiscard]] std::shared_ptr<Table> requireTable(std::string_view tableName) const;

    StorageManager &storage_;
    WriteAheadLog &wal_;
    TxnSession &session_;
    std::shared_ptr<Database> &database_;
    ReplayQuery replayQuery_;
    bool replayingWal_{false};
};

} // namespace VertexDB
