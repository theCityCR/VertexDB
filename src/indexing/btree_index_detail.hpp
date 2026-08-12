#pragma once

// Internal B+ tree key-search helpers. Used by btree_index_{lookup,mutate,snapshot}.cpp.
// Not part of the public include surface.

#include "VertexDB/common/value.hpp"

#include <cstddef>
#include <vector>

namespace VertexDB {
namespace btree_detail {

[[nodiscard]] inline std::size_t lowerBoundKey(const std::vector<Value> &keys, const Value &key) {
    std::size_t lo = 0;
    std::size_t hi = keys.size();
    while (lo < hi) {
        const std::size_t mid = lo + (hi - lo) / 2;
        if (keys[mid] < key) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

} // namespace btree_detail
} // namespace VertexDB
