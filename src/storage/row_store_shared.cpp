#include "VertexDB/storage/row_store.hpp"

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

} // namespace VertexDB
