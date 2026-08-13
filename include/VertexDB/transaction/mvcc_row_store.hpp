#pragma once

// Per-row version chains for commit-aware snapshot isolation reads.
// TransactionManager supplies commit sequences; UndoLog handles abort compensation.

#include "VertexDB/storage/row.hpp"
#include "VertexDB/transaction/transaction_manager.hpp"

#include <functional>
#include <map>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace VertexDB {

struct RowVersion {
    TransactionId createdBy{};
    std::optional<TransactionId> deletedBy;
    Row row;
};

class MVCCRowStore {
  public:
    void write(RowId rowId, Row row, TransactionId transactionId);
    void erase(RowId rowId, TransactionId transactionId);
    void clear();
    // Compensating helpers for undo-log rollback of Table mutations.
    [[nodiscard]] bool popLatestVersion(RowId rowId);
    [[nodiscard]] bool clearLatestDeletedBy(RowId rowId);
    [[nodiscard]] std::optional<Row> read(RowId rowId, const ReadSnapshot &snapshot,
                                          const TransactionManager &transactions) const;
    [[nodiscard]] std::vector<Row> visibleRows(const ReadSnapshot &snapshot,
                                               const TransactionManager &transactions) const;
    [[nodiscard]] std::vector<Row> visibleRowsById(std::span<const RowId> rowIds,
                                                   const ReadSnapshot &snapshot,
                                                   const TransactionManager &transactions) const;
    [[nodiscard]] std::vector<std::pair<RowId, Row>>
    visibleEntriesById(std::span<const RowId> rowIds, const ReadSnapshot &snapshot,
                       const TransactionManager &transactions) const;
    [[nodiscard]] std::vector<std::pair<RowId, Row>>
    visibleEntries(const ReadSnapshot &snapshot, const TransactionManager &transactions) const;
    [[nodiscard]] std::size_t versionCount(RowId rowId) const;
    // Rewrite every version image (ALTER TABLE ADD/DROP COLUMN).
    void transformRows(const std::function<void(Row &)> &fn);
    // Remove columnIndex from every version; returns per-rowId values in chain order.
    [[nodiscard]] std::vector<std::pair<RowId, std::vector<Value>>>
    extractColumn(std::size_t columnIndex);
    // Inverse of extractColumn for ALTER DROP COLUMN undo.
    void insertColumn(std::size_t columnIndex,
                      const std::vector<std::pair<RowId, std::vector<Value>>> &values);

  private:
    std::map<RowId, std::vector<RowVersion>> versions_;
};

} // namespace VertexDB
