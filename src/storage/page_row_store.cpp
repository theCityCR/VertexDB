#include "VertexDB/storage/page_row_store.hpp"

#include <algorithm>
#include <stdexcept>

namespace VertexDB {

PageRowStore::PageRowStore(std::size_t rowsPerPage, std::size_t bufferPageCapacity)
    : rowsPerPage_(rowsPerPage), bufferPool_(bufferPageCapacity) {
    if (rowsPerPage_ == 0) {
        throw std::invalid_argument("rows per page must be positive");
    }
}

RowId PageRowStore::append(Row row) {
    RowId rowId = 0;
    if (!freeList_.empty()) {
        rowId = freeList_.back();
        freeList_.pop_back();
        auto &slot = slots_[rowId];
        slot.live = true;
        auto pageRows = loadPageRows(slot.pageId);
        if (slot.offset >= pageRows.size()) {
            throw std::runtime_error("page slot offset out of range on free-list reuse");
        }
        pageRows[slot.offset] = std::move(row);
        storePage(slot.pageId, pageRows);
    } else {
        rowId = slots_.size();
        const PageId pageId = rowId / rowsPerPage_ + 1;
        auto pageRows = loadPageRows(pageId);
        pageRows.push_back(std::move(row));
        slots_.push_back(Slot{pageId, pageRows.size() - 1, true});
        storePage(pageId, pageRows);
    }
    ++liveCount_;
    return rowId;
}

bool PageRowStore::erase(RowId rowId) {
    if (rowId >= slots_.size() || !slots_[rowId].live) {
        return false;
    }
    auto &slot = slots_[rowId];
    slot.live = false;
    auto pageRows = loadPageRows(slot.pageId);
    if (slot.offset >= pageRows.size()) {
        throw std::runtime_error("page slot offset out of range on erase");
    }
    pageRows[slot.offset] = Row{};
    storePage(slot.pageId, pageRows);
    freeList_.push_back(rowId);
    --liveCount_;
    return true;
}

bool PageRowStore::update(RowId rowId, Row row) {
    if (rowId >= slots_.size() || !slots_[rowId].live) {
        return false;
    }
    auto &slot = slots_[rowId];
    auto pageRows = loadPageRows(slot.pageId);
    if (slot.offset >= pageRows.size()) {
        throw std::runtime_error("page slot offset out of range on update");
    }
    pageRows[slot.offset] = std::move(row);
    storePage(slot.pageId, pageRows);
    return true;
}

bool PageRowStore::revive(RowId rowId, Row row) {
    if (rowId >= slots_.size() || slots_[rowId].live) {
        return false;
    }
    const auto freeIt = std::find(freeList_.begin(), freeList_.end(), rowId);
    if (freeIt == freeList_.end()) {
        return false;
    }
    freeList_.erase(freeIt);

    auto &slot = slots_[rowId];
    slot.live = true;
    auto pageRows = loadPageRows(slot.pageId);
    if (slot.offset >= pageRows.size()) {
        throw std::runtime_error("page slot offset out of range on revive");
    }
    pageRows[slot.offset] = std::move(row);
    storePage(slot.pageId, pageRows);
    ++liveCount_;
    return true;
}

bool PageRowStore::upsertAt(RowId rowId, Row row) {
    if (rowId < slots_.size()) {
        if (slots_[rowId].live) {
            return update(rowId, std::move(row));
        }
        return revive(rowId, std::move(row));
    }
    if (rowId != slots_.size()) {
        return false;
    }
    const PageId pageId = rowId / rowsPerPage_ + 1;
    auto pageRows = loadPageRows(pageId);
    pageRows.push_back(std::move(row));
    slots_.push_back(Slot{pageId, pageRows.size() - 1, true});
    storePage(pageId, pageRows);
    ++liveCount_;
    return true;
}

const Row *PageRowStore::get(RowId rowId) const {
    if (rowId >= slots_.size() || !slots_[rowId].live) {
        return nullptr;
    }

    const auto &slot = slots_[rowId];
    ensureBuffered(slot.pageId);

    if (auto it = decodedRows_.find(rowId); it != decodedRows_.end()) {
        return &it->second;
    }

    const auto buffered = bufferPool_.get(slot.pageId);
    if (!buffered.has_value()) {
        return nullptr;
    }
    const auto pageRows = decodePage(buffered->bytes);
    if (slot.offset >= pageRows.size()) {
        return nullptr;
    }
    auto [it, _] = decodedRows_.emplace(rowId, pageRows[slot.offset]);
    return &it->second;
}

std::vector<Row> PageRowStore::snapshot() const {
    std::vector<Row> rows;
    rows.reserve(liveCount_);
    for (RowId rowId = 0; rowId < slots_.size(); ++rowId) {
        if (const auto *row = get(rowId); row != nullptr) {
            rows.push_back(*row);
        }
    }
    return rows;
}

std::vector<std::pair<RowId, Row>> PageRowStore::liveEntries() const {
    std::vector<std::pair<RowId, Row>> entries;
    entries.reserve(liveCount_);
    for (RowId rowId = 0; rowId < slots_.size(); ++rowId) {
        if (const auto *row = get(rowId); row != nullptr) {
            entries.emplace_back(rowId, *row);
        }
    }
    return entries;
}

std::vector<RowId> PageRowStore::freeList() const { return freeList_; }

std::vector<Row> PageRowStore::rowsById(std::span<const RowId> rowIds) const {
    std::vector<Row> rows;
    rows.reserve(rowIds.size());
    for (const auto rowId : rowIds) {
        if (const auto *row = get(rowId); row != nullptr) {
            rows.push_back(*row);
        }
    }
    return rows;
}

std::size_t PageRowStore::size() const noexcept { return liveCount_; }

std::size_t PageRowStore::capacity() const noexcept { return slots_.size(); }

PageId PageRowStore::pageIdFor(RowId rowId) const {
    if (rowId >= slots_.size()) {
        throw std::out_of_range("row id out of range");
    }
    return slots_[rowId].pageId;
}

std::size_t PageRowStore::rowsPerPage() const noexcept { return rowsPerPage_; }

std::unique_ptr<RowStore> makePageRowStore() { return std::make_unique<PageRowStore>(); }

} // namespace VertexDB
