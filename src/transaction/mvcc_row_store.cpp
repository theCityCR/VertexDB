#include "VertexDB/transaction/mvcc_row_store.hpp"

#include <algorithm>

namespace VertexDB {

void MVCCRowStore::write(RowId rowId, Row row, TransactionId transactionId) {
    versions_[rowId].push_back({transactionId, std::nullopt, std::move(row)});
}

void MVCCRowStore::erase(RowId rowId, TransactionId transactionId) {
    auto it = versions_.find(rowId);
    if (it == versions_.end()) {
        return;
    }
    if (!it->second.empty()) {
        it->second.back().deletedBy = transactionId;
    }
}

void MVCCRowStore::clear() { versions_.clear(); }

std::optional<Row> MVCCRowStore::read(RowId rowId, TransactionId readerId) const {
    auto it = versions_.find(rowId);
    if (it == versions_.end()) {
        return std::nullopt;
    }
    for (auto version = it->second.rbegin(); version != it->second.rend(); ++version) {
        if (version->createdBy <= readerId &&
            (!version->deletedBy || *version->deletedBy > readerId)) {
            return version->row;
        }
    }
    return std::nullopt;
}

std::vector<Row> MVCCRowStore::visibleRows(TransactionId readerId) const {
    std::vector<Row> rows;
    rows.reserve(versions_.size());
    for (const auto &[rowId, _] : versions_) {
        if (auto row = read(rowId, readerId)) {
            rows.push_back(std::move(*row));
        }
    }
    return rows;
}

std::vector<Row> MVCCRowStore::visibleRowsById(std::span<const RowId> rowIds,
                                               TransactionId readerId) const {
    std::vector<Row> rows;
    rows.reserve(rowIds.size());
    for (const auto rowId : rowIds) {
        if (auto row = read(rowId, readerId)) {
            rows.push_back(std::move(*row));
        }
    }
    return rows;
}

std::size_t MVCCRowStore::versionCount(RowId rowId) const {
    auto it = versions_.find(rowId);
    if (it == versions_.end()) {
        return 0;
    }
    return it->second.size();
}

} // namespace VertexDB
