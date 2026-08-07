#pragma once

// Page-image DML redo encoding/decoding for WAL (heap + index dirty pages).
// Legacy row after-images live in physical_redo.hpp.

#include "VertexDB/indexing/btree_index.hpp"
#include "VertexDB/indexing/hash_index.hpp"
#include "VertexDB/storage/buffer_pool.hpp"
#include "VertexDB/storage/row.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace VertexDB {

enum class IndexPageKind : std::uint8_t {
    Hash = 0,
    BTree = 1,
};

struct HeapPageImage {
    PageId pageId{};
    std::vector<std::byte> bytes;
};

struct IndexPageImage {
    std::string indexName;
    IndexPageKind kind{IndexPageKind::Hash};
    // BTree: node page id. Hash: unused (0). Meta blobs use pageId == 0 with empty/special encoding
    // inside the kind-specific snapshot helpers below.
    std::uint64_t pageId{};
    std::vector<std::byte> blob;
};

struct PageImageRedoRecord {
    std::string tableName;
    bool hasHeapMeta{false};
    std::uint64_t capacity{};
    std::vector<RowId> freeList;
    std::vector<HeapPageImage> heapPages;
    // Touched indexes: full B+ tree page set and/or hash bucket images.
    std::vector<std::pair<std::string, BTreeIndexSnapshot>> btreeIndexes;
    std::vector<std::pair<std::string, HashIndexSnapshot>> hashIndexes;
};

[[nodiscard]] std::string encodePageImageRedos(std::span<const PageImageRedoRecord> records);
[[nodiscard]] std::vector<PageImageRedoRecord> decodePageImageRedos(std::string_view payload);

[[nodiscard]] inline std::string encodePageImageRedo(const PageImageRedoRecord &record) {
    return encodePageImageRedos(std::span<const PageImageRedoRecord>{&record, 1});
}

} // namespace VertexDB
