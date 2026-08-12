#include "VertexDB/storage/vector_row_store.hpp"

#include <algorithm>
#include <stdexcept>

namespace VertexDB {

RowId VectorRowStore::append(Row row) {
    RowId rowId = 0;
    if (!freeList_.empty()) {
        rowId = freeList_.back();
        freeList_.pop_back();
        rows_[rowId] = std::move(row);
    } else {
        rowId = rows_.size();
        rows_.push_back(std::move(row));
    }
    ++liveCount_;
    return rowId;
}

bool VectorRowStore::erase(RowId rowId) {
    if (rowId >= rows_.size() || !rows_[rowId].has_value()) {
        return false;
    }
    rows_[rowId].reset();
    freeList_.push_back(rowId);
    --liveCount_;
    return true;
}

bool VectorRowStore::update(RowId rowId, Row row) {
    if (rowId >= rows_.size() || !rows_[rowId].has_value()) {
        return false;
    }
    rows_[rowId] = std::move(row);
    return true;
}

bool VectorRowStore::revive(RowId rowId, Row row) {
    if (rowId >= rows_.size() || rows_[rowId].has_value()) {
        return false;
    }
    const auto freeIt = std::find(freeList_.begin(), freeList_.end(), rowId);
    if (freeIt == freeList_.end()) {
        return false;
    }
    freeList_.erase(freeIt);
    rows_[rowId] = std::move(row);
    ++liveCount_;
    return true;
}

bool VectorRowStore::upsertAt(RowId rowId, Row row) {
    if (rowId < rows_.size()) {
        if (rows_[rowId].has_value()) {
            return update(rowId, std::move(row));
        }
        return revive(rowId, std::move(row));
    }
    if (rowId != rows_.size()) {
        return false;
    }
    rows_.push_back(std::move(row));
    ++liveCount_;
    return true;
}

const Row *VectorRowStore::get(RowId rowId) const {
    if (rowId >= rows_.size() || !rows_[rowId].has_value()) {
        return nullptr;
    }
    return &*rows_[rowId];
}

std::vector<Row> VectorRowStore::snapshot() const {
    std::vector<Row> rows;
    rows.reserve(liveCount_);
    for (const auto &row : rows_) {
        if (row.has_value()) {
            rows.push_back(*row);
        }
    }
    return rows;
}

std::vector<std::pair<RowId, Row>> VectorRowStore::liveEntries() const {
    std::vector<std::pair<RowId, Row>> entries;
    entries.reserve(liveCount_);
    for (RowId rowId = 0; rowId < rows_.size(); ++rowId) {
        if (rows_[rowId].has_value()) {
            entries.emplace_back(rowId, *rows_[rowId]);
        }
    }
    return entries;
}

std::vector<RowId> VectorRowStore::freeList() const { return freeList_; }

std::vector<Row> VectorRowStore::rowsById(std::span<const RowId> rowIds) const {
    std::vector<Row> rows;
    rows.reserve(rowIds.size());
    for (const auto rowId : rowIds) {
        if (const auto *row = get(rowId); row != nullptr) {
            rows.push_back(*row);
        }
    }
    return rows;
}

std::size_t VectorRowStore::size() const noexcept { return liveCount_; }

std::size_t VectorRowStore::capacity() const noexcept { return rows_.size(); }

void VectorRowStore::replaceRows(std::vector<Row> rows) {
    rows_.clear();
    freeList_.clear();
    liveCount_ = 0;
    rows_.reserve(rows.size());
    for (auto &row : rows) {
        (void)append(std::move(row));
    }
}

void VectorRowStore::replaceSparse(std::size_t capacity, std::vector<RowId> freeList,
                                  std::vector<std::pair<RowId, Row>> entries) {
    validateSparseRowLayout(capacity, freeList, entries);
    rows_.assign(capacity, std::nullopt);
    freeList_ = std::move(freeList);
    liveCount_ = entries.size();
    for (auto &[rowId, row] : entries) {
        rows_[rowId] = std::move(row);
    }
}

std::unique_ptr<RowStore> makeVectorRowStore() { return std::make_unique<VectorRowStore>(); }

} // namespace VertexDB
