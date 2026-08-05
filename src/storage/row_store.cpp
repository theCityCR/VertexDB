#include "VertexDB/storage/row_store.hpp"

#include <cstring>
#include <stdexcept>
#include <unordered_set>

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

template <typename T> T readPod(std::span<const std::byte> &bytes) {
    if (bytes.size() < sizeof(T)) {
        throw std::runtime_error("truncated page payload while reading fixed-width field");
    }
    T value{};
    std::memcpy(&value, bytes.data(), sizeof(T));
    bytes = bytes.subspan(sizeof(T));
    return value;
}

Value readValue(std::span<const std::byte> &bytes) {
    const auto typeTag = readPod<std::uint8_t>(bytes);
    if (typeTag == 255) {
        return Value{};
    }

    switch (static_cast<ColumnType>(typeTag)) {
    case ColumnType::Int:
        return Value{readPod<std::int64_t>(bytes)};
    case ColumnType::Double:
        return Value{readPod<double>(bytes)};
    case ColumnType::String: {
        const auto size = readPod<std::uint64_t>(bytes);
        if (bytes.size() < size) {
            throw std::runtime_error("truncated page payload while reading string");
        }
        std::string text(reinterpret_cast<const char *>(bytes.data()),
                         static_cast<std::size_t>(size));
        bytes = bytes.subspan(static_cast<std::size_t>(size));
        return Value{std::move(text)};
    }
    }

    throw std::runtime_error("invalid value type tag in page payload");
}

void validateSparseLayout(std::size_t capacity, const std::vector<RowId> &freeList,
                          const std::vector<std::pair<RowId, Row>> &entries) {
    if (entries.size() > capacity) {
        throw std::invalid_argument("sparse row layout has more live rows than capacity");
    }
    if (freeList.size() + entries.size() != capacity) {
        throw std::invalid_argument("sparse row layout free list and live rows must cover capacity");
    }

    std::unordered_set<RowId> seen;
    seen.reserve(capacity);
    for (const auto &[rowId, _] : entries) {
        if (rowId >= capacity) {
            throw std::invalid_argument("sparse live row id exceeds capacity");
        }
        if (!seen.insert(rowId).second) {
            throw std::invalid_argument("duplicate sparse live row id");
        }
    }
    for (const auto rowId : freeList) {
        if (rowId >= capacity) {
            throw std::invalid_argument("sparse free-list row id exceeds capacity");
        }
        if (!seen.insert(rowId).second) {
            throw std::invalid_argument("sparse free-list overlaps a live or duplicate row id");
        }
    }
    if (seen.size() != capacity) {
        throw std::invalid_argument("sparse row layout does not cover every slot");
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
    validateSparseLayout(capacity, freeList, entries);
    rows_.assign(capacity, std::nullopt);
    freeList_ = std::move(freeList);
    liveCount_ = entries.size();
    for (auto &[rowId, row] : entries) {
        rows_[rowId] = std::move(row);
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

bool PageRowStore::bufferContains(PageId pageId) const { return bufferPool_.contains(pageId); }

std::size_t PageRowStore::bufferSize() const noexcept { return bufferPool_.size(); }

PageId PageRowStore::pageIdFor(RowId rowId) const {
    if (rowId >= slots_.size()) {
        throw std::out_of_range("row id out of range");
    }
    return slots_[rowId].pageId;
}

std::optional<std::vector<std::byte>> PageRowStore::directoryBytes(PageId pageId) const {
    const auto it = pageDirectory_.find(pageId);
    if (it == pageDirectory_.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::vector<Row> PageRowStore::decodePage(std::span<const std::byte> bytes) {
    auto cursor = bytes;
    const auto rowCount = readPod<std::uint64_t>(cursor);
    std::vector<Row> rows;
    rows.reserve(static_cast<std::size_t>(rowCount));
    for (std::uint64_t i = 0; i < rowCount; ++i) {
        const auto columnCount = readPod<std::uint64_t>(cursor);
        Row row;
        row.reserve(static_cast<std::size_t>(columnCount));
        for (std::uint64_t c = 0; c < columnCount; ++c) {
            row.push_back(readValue(cursor));
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

void PageRowStore::replaceRows(std::vector<Row> rows) {
    pageDirectory_.clear();
    slots_.clear();
    freeList_.clear();
    decodedRows_.clear();
    liveCount_ = 0;
    for (auto &row : rows) {
        (void)append(std::move(row));
    }
}

void PageRowStore::replaceSparse(std::size_t capacity, std::vector<RowId> freeList,
                                std::vector<std::pair<RowId, Row>> entries) {
    validateSparseLayout(capacity, freeList, entries);

    pageDirectory_.clear();
    decodedRows_.clear();
    slots_.assign(capacity, Slot{});
    freeList_ = std::move(freeList);
    liveCount_ = entries.size();

    std::vector<bool> live(capacity, false);
    std::vector<Row> liveRows(capacity);
    for (auto &[rowId, row] : entries) {
        live[rowId] = true;
        liveRows[rowId] = std::move(row);
    }

    std::unordered_map<PageId, std::vector<Row>> pages;
    for (RowId rowId = 0; rowId < capacity; ++rowId) {
        const PageId pageId = rowId / rowsPerPage_ + 1;
        auto &pageRows = pages[pageId];
        const auto offset = pageRows.size();
        pageRows.push_back(live[rowId] ? std::move(liveRows[rowId]) : Row{});
        slots_[rowId] = Slot{pageId, offset, live[rowId]};
    }

    for (const auto &[pageId, pageRows] : pages) {
        storePage(pageId, pageRows);
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

std::vector<Row> PageRowStore::loadPageRows(PageId pageId) const {
    const auto it = pageDirectory_.find(pageId);
    if (it == pageDirectory_.end()) {
        return {};
    }
    return decodePage(it->second);
}

void PageRowStore::storePage(PageId pageId, const std::vector<Row> &rows) {
    auto page = serializePage(pageId, rows);
    pageDirectory_[pageId] = page.bytes;
    bufferPool_.put(std::move(page));
    invalidateDecoded(pageId);
}

void PageRowStore::ensureBuffered(PageId pageId) const {
    if (bufferPool_.contains(pageId)) {
        return;
    }
    const auto it = pageDirectory_.find(pageId);
    if (it == pageDirectory_.end()) {
        return;
    }
    bufferPool_.put(Page{pageId, it->second, false});
}

void PageRowStore::invalidateDecoded(PageId pageId) {
    for (RowId rowId = 0; rowId < slots_.size(); ++rowId) {
        if (slots_[rowId].pageId == pageId) {
            decodedRows_.erase(rowId);
        }
    }
}

std::unique_ptr<RowStore> makePageRowStore() { return std::make_unique<PageRowStore>(); }

} // namespace VertexDB
