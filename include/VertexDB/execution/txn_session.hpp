#pragma once

// Per-executor transaction state: MVCC snapshots, undo, and deferred WAL records.

#include "VertexDB/execution/query_result.hpp"
#include "VertexDB/persistence/write_ahead_log.hpp"
#include "VertexDB/transaction/transaction_manager.hpp"
#include "VertexDB/transaction/undo_log.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace VertexDB {

struct PendingWalRecord {
    WalOperation operation{};
    std::string payload;
};

class TxnSession {
  public:
    [[nodiscard]] QueryResult begin();
    [[nodiscard]] QueryResult commit();
    [[nodiscard]] QueryResult rollback();
    void reset() noexcept;

    [[nodiscard]] ReadSnapshot readSnapshot() const;
    [[nodiscard]] TransactionId writeTransactionId();
    [[nodiscard]] bool transactionActive() const noexcept;
    [[nodiscard]] std::optional<TransactionId> activeTransactionId() const noexcept;
    [[nodiscard]] QueryResult rejectIfTransactionActive(std::string_view action) const;

    [[nodiscard]] TransactionManager &transactionManager() noexcept;
    [[nodiscard]] const TransactionManager &transactionManager() const noexcept;
    [[nodiscard]] UndoLog &undoLog() noexcept;
    [[nodiscard]] const UndoLog &undoLog() const noexcept;

    void pushUndo(UndoRecord record);
    void pushPendingWal(PendingWalRecord record);
    [[nodiscard]] std::vector<PendingWalRecord> &pendingWal() noexcept;
    [[nodiscard]] const std::vector<PendingWalRecord> &pendingWal() const noexcept;
    void clearPendingWal() noexcept;
    void rewriteTableName(std::string_view oldName, std::string_view newName);

  private:
    TransactionManager transactionManager_;
    UndoLog undoLog_;
    std::optional<TransactionId> activeTransaction_;
    std::optional<ReadSnapshot> activeSnapshot_;
    std::vector<PendingWalRecord> pendingWal_;
};

} // namespace VertexDB
