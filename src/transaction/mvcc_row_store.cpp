#include "VertexDB/transaction/mvcc_row_store.hpp"

#include <stdexcept>
#include <utility>

namespace VertexDB {

void MVCCRowStore::write(RowId rowId, Row row, TransactionId transactionId) {
    auto &chain = versions_[rowId];
    // Close the prior live version so UPDATE then DELETE cannot resurrect the pre-update image.
    if (!chain.empty() && !chain.back().deletedBy) {
        chain.back().deletedBy = transactionId;
    }
    chain.push_back({transactionId, std::nullopt, std::move(row)});
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
        return true;
    }
    // Undoing an UPDATE must revive the prior version that write() had closed.
    it->second.back().deletedBy.reset();
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

void MVCCRowStore::transformRows(const std::function<void(Row &)> &fn) {
    for (auto &[_, chain] : versions_) {
        for (auto &version : chain) {
            fn(version.row);
        }
    }
}

std::vector<std::pair<RowId, std::vector<Value>>>
MVCCRowStore::extractColumn(std::size_t columnIndex) {
    std::vector<std::pair<RowId, std::vector<Value>>> out;
    out.reserve(versions_.size());
    for (auto &[rowId, chain] : versions_) {
        std::vector<Value> values;
        values.reserve(chain.size());
        for (auto &version : chain) {
            if (columnIndex >= version.row.size()) {
                throw std::runtime_error("MVCC row narrower than schema during ALTER");
            }
            values.push_back(version.row[columnIndex]);
            version.row.erase(version.row.begin() +
                              static_cast<std::ptrdiff_t>(columnIndex));
        }
        out.emplace_back(rowId, std::move(values));
    }
    return out;
}

void MVCCRowStore::insertColumn(std::size_t columnIndex,
                                const std::vector<std::pair<RowId, std::vector<Value>>> &values) {
    std::map<RowId, std::vector<Value>> byId;
    for (const auto &[rowId, columnValues] : values) {
        byId.emplace(rowId, columnValues);
    }
    for (auto &[rowId, chain] : versions_) {
        const auto it = byId.find(rowId);
        if (it == byId.end() || it->second.size() != chain.size()) {
            throw std::runtime_error("ALTER undo version column values mismatch");
        }
        for (std::size_t i = 0; i < chain.size(); ++i) {
            if (columnIndex > chain[i].row.size()) {
                throw std::runtime_error("ALTER undo column index out of range");
            }
            chain[i].row.insert(chain[i].row.begin() + static_cast<std::ptrdiff_t>(columnIndex),
                                it->second[i]);
        }
    }
}

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
MVCCRowStore::visibleEntriesById(std::span<const RowId> rowIds, const ReadSnapshot &snapshot,
                                 const TransactionManager &transactions) const {
    std::vector<std::pair<RowId, Row>> entries;
    entries.reserve(rowIds.size());
    for (const auto rowId : rowIds) {
        if (auto row = read(rowId, snapshot, transactions)) {
            entries.emplace_back(rowId, std::move(*row));
        }
    }
    return entries;
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
