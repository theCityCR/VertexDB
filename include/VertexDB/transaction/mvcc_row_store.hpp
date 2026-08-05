#pragma once

#include "VertexDB/storage/row.hpp"
#include "VertexDB/transaction/transaction_manager.hpp"

#include <map>
#include <optional>
#include <span>
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
    [[nodiscard]] std::optional<Row> read(RowId rowId, TransactionId readerId) const;
    [[nodiscard]] std::vector<Row> visibleRows(TransactionId readerId) const;
    [[nodiscard]] std::vector<Row> visibleRowsById(std::span<const RowId> rowIds,
                                                   TransactionId readerId) const;
    [[nodiscard]] std::size_t versionCount(RowId rowId) const;

  private:
    std::map<RowId, std::vector<RowVersion>> versions_;
};

} // namespace VertexDB
