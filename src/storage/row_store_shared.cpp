#include "VertexDB/storage/row_store.hpp"

#include <algorithm>
#include <stdexcept>
#include <unordered_set>

namespace VertexDB {

void validateSparseRowLayout(std::size_t capacity, const std::vector<RowId> &freeList,
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

void validatePageStoreLayout(const PageStoreSnapshot &snapshot) {
    if (snapshot.rowsPerPage == 0) {
        throw std::invalid_argument("page store snapshot rowsPerPage must be positive");
    }
    if (snapshot.freeList.size() > snapshot.capacity) {
        throw std::invalid_argument("page store snapshot free list exceeds capacity");
    }

    std::unordered_set<RowId> freeIds;
    freeIds.reserve(snapshot.freeList.size());
    for (const auto rowId : snapshot.freeList) {
        if (rowId >= snapshot.capacity) {
            throw std::invalid_argument("page store snapshot free-list row id exceeds capacity");
        }
        if (!freeIds.insert(rowId).second) {
            throw std::invalid_argument("page store snapshot free list has duplicate row id");
        }
    }

    const std::size_t expectedPageCount =
        snapshot.capacity == 0
            ? 0
            : (snapshot.capacity + snapshot.rowsPerPage - 1) / snapshot.rowsPerPage;
    if (snapshot.pages.size() != expectedPageCount) {
        throw std::invalid_argument("page store snapshot page count does not match capacity");
    }

    std::unordered_set<PageId> seenPages;
    seenPages.reserve(snapshot.pages.size());
    for (std::size_t index = 0; index < snapshot.pages.size(); ++index) {
        const auto &[pageId, bytes] = snapshot.pages[index];
        const PageId expectedPageId = static_cast<PageId>(index + 1);
        if (pageId != expectedPageId) {
            throw std::invalid_argument("page store snapshot pages must be contiguous from 1");
        }
        if (!seenPages.insert(pageId).second) {
            throw std::invalid_argument("page store snapshot has duplicate page id");
        }

        const auto pageRows = PageRowStore::decodePage(bytes);
        const std::size_t firstRowId = static_cast<std::size_t>(pageId - 1) * snapshot.rowsPerPage;
        const std::size_t expectedSlots =
            std::min(snapshot.rowsPerPage, snapshot.capacity - firstRowId);
        if (pageRows.size() != expectedSlots) {
            throw std::invalid_argument("page store snapshot page slot count does not match layout");
        }
        for (std::size_t offset = 0; offset < pageRows.size(); ++offset) {
            const RowId rowId = static_cast<RowId>(firstRowId + offset);
            const bool isFree = freeIds.contains(rowId);
            if (isFree) {
                if (!pageRows[offset].empty()) {
                    throw std::invalid_argument(
                        "page store snapshot free slot must hold an empty tombstone row");
                }
            } else if (pageRows[offset].empty()) {
                throw std::invalid_argument("page store snapshot live slot cannot be empty");
            }
        }
    }
}

} // namespace VertexDB
