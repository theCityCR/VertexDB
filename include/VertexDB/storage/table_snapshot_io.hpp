#pragma once

// Snapshot/redo collaborator for Table. Its owning Table provides synchronization
// and passes Table-owned row, index, MVCC, and schema state to each operation.

#include "VertexDB/indexing/index_manager.hpp"
#include "VertexDB/storage/row_store.hpp"
#include "VertexDB/transaction/mvcc_row_store.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace VertexDB {

struct IndexStoreSnapshot {
    std::string name;
    std::string column;
    BTreeIndexSnapshot btree;
    HashIndexSnapshot hash;
};

struct TableIndexStoreSnapshot {
    std::vector<IndexStoreSnapshot> indexes;
};

struct PageImageCapture {
    std::size_t capacity{};
    std::vector<RowId> freeList;
    std::vector<std::pair<PageId, std::vector<std::byte>>> heapPages;
    std::vector<std::pair<std::string, BTreeIndexSnapshot>> btreeIndexes;
    std::vector<std::pair<std::string, HashIndexSnapshot>> hashIndexes;
};

class TableSnapshotIO {
  public:
    bool applyPhysicalUpsert(RowStore &rowStore, IndexManager &indexes,
                             MVCCRowStore &versions, std::span<const Column> schema,
                             RowId rowId, Row row);
    bool applyPhysicalErase(RowStore &rowStore, IndexManager &indexes,
                            MVCCRowStore &versions, std::span<const Column> schema,
                            RowId rowId);
    void applyPageImageRedo(
        RowStore &rowStore, IndexManager &indexes, MVCCRowStore &versions, bool hasHeapMeta,
        std::size_t capacity, std::vector<RowId> freeList,
        std::vector<std::pair<PageId, std::vector<std::byte>>> heapPages,
        std::vector<std::pair<std::string, BTreeIndexSnapshot>> btreeIndexes,
        std::vector<std::pair<std::string, HashIndexSnapshot>> hashIndexes);

    void replaceRows(RowStore &rowStore, IndexManager &indexes, MVCCRowStore &versions,
                     std::span<const Column> schema, std::vector<Row> rows);
    void replaceSparse(RowStore &rowStore, IndexManager &indexes, MVCCRowStore &versions,
                       std::span<const Column> schema, std::size_t capacity,
                       std::vector<RowId> freeList,
                       std::vector<std::pair<RowId, Row>> entries);
    [[nodiscard]] PageStoreSnapshot exportPageStore(const RowStore &rowStore) const;
    void replaceFromPages(RowStore &rowStore, IndexManager &indexes,
                          MVCCRowStore &versions, std::span<const Column> schema,
                          PageStoreSnapshot snapshot, bool rebuildIndexesAfter);
    [[nodiscard]] TableIndexStoreSnapshot
    exportIndexPages(const IndexManager &indexes, std::span<const Column> schema) const;
    void replaceIndexPages(IndexManager &indexes, TableIndexStoreSnapshot snapshot);
    void clearDirtyTracking(RowStore &rowStore, IndexManager &indexes);
    [[nodiscard]] PageImageCapture takePageImageCapture(RowStore &rowStore,
                                                        IndexManager &indexes);

  private:
    void refreshVersionsFromStore(const RowStore &rowStore, MVCCRowStore &versions);
};

} // namespace VertexDB
