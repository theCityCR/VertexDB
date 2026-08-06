#include "VertexDB/transaction/mvcc_row_store.hpp"

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

bool MVCCRowStore::popLatestVersion(RowId rowId) {
    auto it = versions_.find(rowId);
    if (it == versions_.end() || it->second.empty()) {
        return false;
    }
    it->second.pop_back();
    if (it->second.empty()) {
        versions_.erase(it);
    }
    return true;
}

bool MVCCRowStore::clearLatestDeletedBy(RowId rowId) {
    auto it = versions_.find(rowId);
    if (it == versions_.end() || it->second.empty() || !it->second.back().deletedBy) {
        return false;
    }
    it->second.back().deletedBy.reset();
    return true;
}

void MVCCRowStore::clear() { versions_.clear(); }

std::optional<Row> MVCCRowStore::read(RowId rowId, const ReadSnapshot &snapshot,
                                      const TransactionManager &transactions) const {
    auto it = versions_.find(rowId);
    if (it == versions_.end()) {
        return std::nullopt;
    }
    for (auto version = it->second.rbegin(); version != it->second.rend(); ++version) {
        if (transactions.isVisible(version->createdBy, version->deletedBy, snapshot)) {
            return version->row;
        }
    }
    return std::nullopt;
}

std::vector<Row> MVCCRowStore::visibleRows(const ReadSnapshot &snapshot,
                                           const TransactionManager &transactions) const {
    std::vector<Row> rows;
    rows.reserve(versions_.size());
    for (const auto &[rowId, _] : versions_) {
        if (auto row = read(rowId, snapshot, transactions)) {
            rows.push_back(std::move(*row));
        }
    }
    return rows;
}

std::vector<Row> MVCCRowStore::visibleRowsById(std::span<const RowId> rowIds,
                                               const ReadSnapshot &snapshot,
                                               const TransactionManager &transactions) const {
    std::vector<Row> rows;
    rows.reserve(rowIds.size());
    for (const auto rowId : rowIds) {
        if (auto row = read(rowId, snapshot, transactions)) {
            rows.push_back(std::move(*row));
        }
    }
    return rows;
}

std::vector<std::pair<RowId, Row>>
MVCCRowStore::visibleEntries(const ReadSnapshot &snapshot,
                             const TransactionManager &transactions) const {
    std::vector<std::pair<RowId, Row>> entries;
    entries.reserve(versions_.size());
    for (const auto &[rowId, _] : versions_) {
        if (auto row = read(rowId, snapshot, transactions)) {
            entries.emplace_back(rowId, std::move(*row));
        }
    }
    return entries;
}

std::size_t MVCCRowStore::versionCount(RowId rowId) const {
    auto it = versions_.find(rowId);
    if (it == versions_.end()) {
        return 0;
    }
    return it->second.size();
}

} // namespace VertexDB
