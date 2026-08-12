#include "VertexDB/storage/page_row_store.hpp"

#include "VertexDB/common/binary_io.hpp"

#include <algorithm>
#include <stdexcept>
#include <unordered_map>
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

Value readValue(std::span<const std::byte> &bytes) {
    constexpr std::string_view kTruncated = "truncated page payload while reading fixed-width field";
    const auto typeTag = readPod<std::uint8_t>(bytes, kTruncated);
    if (typeTag == 255) {
        return Value{};
    }

    switch (static_cast<ColumnType>(typeTag)) {
    case ColumnType::Int:
        return Value{readPod<std::int64_t>(bytes, kTruncated)};
    case ColumnType::Double:
        return Value{readPod<double>(bytes, kTruncated)};
    case ColumnType::String: {
        const auto size = readPod<std::uint64_t>(bytes, kTruncated);
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

} // namespace

std::vector<Row> PageRowStore::decodePage(std::span<const std::byte> bytes) {
    constexpr std::string_view kTruncated = "truncated page payload while reading fixed-width field";
    auto cursor = bytes;
    const auto rowCount = readPod<std::uint64_t>(cursor, kTruncated);
    std::vector<Row> rows;
    rows.reserve(static_cast<std::size_t>(rowCount));
    for (std::uint64_t i = 0; i < rowCount; ++i) {
        const auto columnCount = readPod<std::uint64_t>(cursor, kTruncated);
        Row row;
        row.reserve(static_cast<std::size_t>(columnCount));
        for (std::uint64_t c = 0; c < columnCount; ++c) {
            row.push_back(readValue(cursor));
        }
        rows.push_back(std::move(row));
    }
    return rows;
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

PageStoreSnapshot PageRowStore::exportPages() const {
    PageStoreSnapshot snapshot;
    snapshot.rowsPerPage = rowsPerPage_;
    snapshot.capacity = slots_.size();
    snapshot.freeList = freeList_;
    snapshot.pages.reserve(pageDirectory_.size());
    for (const auto &[pageId, bytes] : pageDirectory_) {
        snapshot.pages.emplace_back(pageId, bytes);
    }
    std::sort(snapshot.pages.begin(), snapshot.pages.end(),
              [](const auto &lhs, const auto &rhs) { return lhs.first < rhs.first; });
    return snapshot;
}

void PageRowStore::replaceFromPages(PageStoreSnapshot snapshot) {
    validatePageStoreLayout(snapshot);

    rowsPerPage_ = snapshot.rowsPerPage;
    pageDirectory_.clear();
    decodedRows_.clear();
    slots_.assign(snapshot.capacity, Slot{});
    freeList_ = std::move(snapshot.freeList);
    liveCount_ = snapshot.capacity - freeList_.size();

    std::unordered_set<RowId> freeIds(freeList_.begin(), freeList_.end());
    for (auto &[pageId, bytes] : snapshot.pages) {
        const auto pageRows = decodePage(bytes);
        const std::size_t firstRowId = static_cast<std::size_t>(pageId - 1) * rowsPerPage_;
        for (std::size_t offset = 0; offset < pageRows.size(); ++offset) {
            const RowId rowId = static_cast<RowId>(firstRowId + offset);
            slots_[rowId] = Slot{pageId, offset, !freeIds.contains(rowId)};
        }
        pageDirectory_[pageId] = std::move(bytes);
        bufferPool_.put(Page{pageId, pageDirectory_[pageId], false});
    }
    clearDirtyPages();
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
    validateSparseRowLayout(capacity, freeList, entries);

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

void PageRowStore::rebuildSlotsFromDirectory() {
    std::unordered_set<RowId> freeIds(freeList_.begin(), freeList_.end());
    const std::size_t cap = slots_.size();
    slots_.assign(cap, Slot{});
    liveCount_ = 0;
    for (const auto &[pageId, bytes] : pageDirectory_) {
        const auto pageRows = decodePage(bytes);
        const std::size_t firstRowId = static_cast<std::size_t>(pageId - 1) * rowsPerPage_;
        for (std::size_t offset = 0; offset < pageRows.size(); ++offset) {
            const RowId rowId = static_cast<RowId>(firstRowId + offset);
            if (rowId >= cap) {
                continue;
            }
            const bool live = !freeIds.contains(rowId);
            slots_[rowId] = Slot{pageId, offset, live};
            if (live) {
                ++liveCount_;
            }
        }
    }
}

void PageRowStore::applyPageImages(std::optional<std::size_t> capacity,
                                   std::optional<std::vector<RowId>> freeList,
                                   std::vector<std::pair<PageId, std::vector<std::byte>>> pages) {
    if (capacity.has_value()) {
        if (*capacity < slots_.size()) {
            throw std::invalid_argument("page image capacity cannot shrink below current slots");
        }
        slots_.resize(*capacity);
    }
    if (freeList.has_value()) {
        freeList_ = std::move(*freeList);
    }
    for (auto &[pageId, bytes] : pages) {
        pageDirectory_[pageId] = std::move(bytes);
        bufferPool_.put(Page{pageId, pageDirectory_[pageId], false});
        invalidateDecoded(pageId);
    }
    rebuildSlotsFromDirectory();
    clearDirtyPages();
}

} // namespace VertexDB
