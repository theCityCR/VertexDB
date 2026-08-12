#include "VertexDB/storage/page_row_store.hpp"

#include <algorithm>

namespace VertexDB {

bool PageRowStore::bufferContains(PageId pageId) const { return bufferPool_.contains(pageId); }

std::size_t PageRowStore::bufferSize() const noexcept { return bufferPool_.size(); }

std::optional<std::vector<std::byte>> PageRowStore::directoryBytes(PageId pageId) const {
    const auto it = pageDirectory_.find(pageId);
    if (it == pageDirectory_.end()) {
        return std::nullopt;
    }
    return it->second;
}

void PageRowStore::storePage(PageId pageId, const std::vector<Row> &rows) {
    auto page = serializePage(pageId, rows);
    pageDirectory_[pageId] = page.bytes;
    bufferPool_.put(std::move(page));
    invalidateDecoded(pageId);
    markDirty(pageId);
}

void PageRowStore::markDirty(PageId pageId) { dirtyPages_[pageId] = true; }

void PageRowStore::clearDirtyPages() noexcept { dirtyPages_.clear(); }

bool PageRowStore::hasDirtyPages() const noexcept { return !dirtyPages_.empty(); }

std::vector<std::pair<PageId, std::vector<std::byte>>> PageRowStore::takeDirtyPages() {
    std::vector<std::pair<PageId, std::vector<std::byte>>> pages;
    pages.reserve(dirtyPages_.size());
    for (const auto &[pageId, _] : dirtyPages_) {
        const auto it = pageDirectory_.find(pageId);
        if (it != pageDirectory_.end()) {
            pages.emplace_back(pageId, it->second);
        }
    }
    std::sort(pages.begin(), pages.end(),
              [](const auto &lhs, const auto &rhs) { return lhs.first < rhs.first; });
    dirtyPages_.clear();
    return pages;
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

} // namespace VertexDB
