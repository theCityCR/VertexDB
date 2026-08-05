#include "VertexDB/storage/row_store.hpp"

#include <stdexcept>

namespace VertexDB {
namespace {

template <typename T> void appendBytes(std::vector<std::byte> &bytes, const T &value) {
    const auto *raw = reinterpret_cast<const std::byte *>(&value);
    bytes.insert(bytes.end(), raw, raw + sizeof(T));
}

void appendValue(std::vector<std::byte> &bytes, const Value &value) {
    const auto type =
        static_cast<std::uint8_t>(value.isNull() ? 255 : static_cast<int>(value.type()));
    appendBytes(bytes, type);
    if (value.isNull()) {
        return;
    }

    switch (value.type()) {
    case ColumnType::Int:
        appendBytes(bytes, std::get<std::int64_t>(value.data()));
        break;
    case ColumnType::Double:
        appendBytes(bytes, std::get<double>(value.data()));
        break;
    case ColumnType::String: {
        const auto &text = std::get<std::string>(value.data());
        const auto size = static_cast<std::uint64_t>(text.size());
        appendBytes(bytes, size);
        const auto *raw = reinterpret_cast<const std::byte *>(text.data());
        bytes.insert(bytes.end(), raw, raw + text.size());
        break;
    }
    }
}

} // namespace

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

std::unique_ptr<RowStore> makeVectorRowStore() { return std::make_unique<VectorRowStore>(); }

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
        pages_.at(slot.pageId)[slot.offset] = std::move(row);
        bufferPool_.put(serializePage(slot.pageId, pages_.at(slot.pageId)));
    } else {
        rowId = slots_.size();
        const PageId pageId = rowId / rowsPerPage_ + 1;
        auto &pageRows = pages_[pageId];
        pageRows.push_back(std::move(row));
        slots_.push_back(Slot{pageId, pageRows.size() - 1, true});
        bufferPool_.put(serializePage(pageId, pageRows));
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
    pages_.at(slot.pageId)[slot.offset] = Row{};
    bufferPool_.put(serializePage(slot.pageId, pages_.at(slot.pageId)));
    freeList_.push_back(rowId);
    --liveCount_;
    return true;
}

bool PageRowStore::update(RowId rowId, Row row) {
    if (rowId >= slots_.size() || !slots_[rowId].live) {
        return false;
    }
    auto &slot = slots_[rowId];
    pages_.at(slot.pageId)[slot.offset] = std::move(row);
    bufferPool_.put(serializePage(slot.pageId, pages_.at(slot.pageId)));
    return true;
}

const Row *PageRowStore::get(RowId rowId) const {
    if (rowId >= slots_.size() || !slots_[rowId].live) {
        return nullptr;
    }
    const auto &slot = slots_[rowId];
    auto page = pages_.find(slot.pageId);
    if (page == pages_.end() || slot.offset >= page->second.size()) {
        return nullptr;
    }
    return &page->second[slot.offset];
}

std::vector<Row> PageRowStore::snapshot() const {
    std::vector<Row> rows;
    rows.reserve(liveCount_);
    for (const auto &slot : slots_) {
        if (!slot.live) {
            continue;
        }
        const auto page = pages_.find(slot.pageId);
        if (page != pages_.end() && slot.offset < page->second.size()) {
            rows.push_back(page->second[slot.offset]);
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

void PageRowStore::replaceRows(std::vector<Row> rows) {
    pages_.clear();
    slots_.clear();
    freeList_.clear();
    liveCount_ = 0;
    for (auto &row : rows) {
        (void)append(std::move(row));
    }
}

Page PageRowStore::serializePage(PageId pageId, const std::vector<Row> &rows) const {
    std::vector<std::byte> bytes;
    appendBytes(bytes, static_cast<std::uint64_t>(rows.size()));
    for (const auto &row : rows) {
        appendBytes(bytes, static_cast<std::uint64_t>(row.size()));
        for (const auto &value : row) {
            appendValue(bytes, value);
        }
    }
    return Page{pageId, std::move(bytes), true};
}

std::unique_ptr<RowStore> makePageRowStore() { return std::make_unique<PageRowStore>(); }

} // namespace VertexDB
