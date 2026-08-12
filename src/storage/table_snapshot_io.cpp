#include "VertexDB/storage/table_snapshot_io.hpp"

#include "VertexDB/common/index_expression.hpp"
#include "VertexDB/storage/page_row_store.hpp"

#include <optional>
#include <stdexcept>

namespace VertexDB {

bool TableSnapshotIO::applyPhysicalUpsert(RowStore &rowStore, IndexManager &indexes,
                                          MVCCRowStore &versions,
                                          std::span<const Column> schema, RowId rowId, Row row) {
    const bool existed = rowStore.get(rowId) != nullptr;
    if (!rowStore.upsertAt(rowId, std::move(row))) {
        return false;
    }
    versions.write(rowId, *rowStore.get(rowId), kSystemTransactionId);
    if (existed) {
        indexes.rebuildIndexes(rowStore, schema);
    } else {
        indexes.addRowToIndexes(rowId, rowStore, schema);
    }
    return true;
}

bool TableSnapshotIO::applyPhysicalErase(RowStore &rowStore, IndexManager &indexes,
                                         MVCCRowStore &versions,
                                         std::span<const Column> schema, RowId rowId) {
    if (rowStore.get(rowId) == nullptr) {
        return false;
    }
    if (!rowStore.erase(rowId)) {
        return false;
    }
    (void)versions.popLatestVersion(rowId);
    indexes.rebuildIndexes(rowStore, schema);
    return true;
}

void TableSnapshotIO::applyPageImageRedo(
    RowStore &rowStore, IndexManager &indexes, MVCCRowStore &versions, bool hasHeapMeta,
    std::size_t capacity, std::vector<RowId> freeList,
    std::vector<std::pair<PageId, std::vector<std::byte>>> heapPages,
    std::vector<std::pair<std::string, BTreeIndexSnapshot>> btreeIndexes,
    std::vector<std::pair<std::string, HashIndexSnapshot>> hashIndexes) {
    auto *pageStore = dynamic_cast<PageRowStore *>(&rowStore);
    if (pageStore == nullptr) {
        throw std::runtime_error("table row store does not support page image redo");
    }
    std::optional<std::size_t> capacityOpt;
    std::optional<std::vector<RowId>> freeListOpt;
    if (hasHeapMeta) {
        capacityOpt = capacity;
        freeListOpt = std::move(freeList);
    }
    pageStore->applyPageImages(capacityOpt, std::move(freeListOpt), std::move(heapPages));
    auto &orderedIndexes = indexes.orderedIndexes();
    auto &hashIndexStores = indexes.hashIndexes();
    for (auto &[name, snapshot] : btreeIndexes) {
        auto it = orderedIndexes.find(name);
        if (it == orderedIndexes.end()) {
            throw std::runtime_error("page image redo references unknown btree index " + name);
        }
        it->second.applyDirtyPages(snapshot);
    }
    for (auto &[name, snapshot] : hashIndexes) {
        auto it = hashIndexStores.find(name);
        if (it == hashIndexStores.end()) {
            throw std::runtime_error("page image redo references unknown hash index " + name);
        }
        it->second.applyDirtyBuckets(snapshot);
    }
    refreshVersionsFromStore(rowStore, versions);
}

void TableSnapshotIO::replaceRows(RowStore &rowStore, IndexManager &indexes,
                                  MVCCRowStore &versions, std::span<const Column> schema,
                                  std::vector<Row> rows) {
    rowStore.replaceRows(std::move(rows));
    refreshVersionsFromStore(rowStore, versions);
    indexes.rebuildIndexes(rowStore, schema);
}

void TableSnapshotIO::replaceSparse(RowStore &rowStore, IndexManager &indexes,
                                    MVCCRowStore &versions, std::span<const Column> schema,
                                    std::size_t capacity, std::vector<RowId> freeList,
                                    std::vector<std::pair<RowId, Row>> entries) {
    rowStore.replaceSparse(capacity, std::move(freeList), std::move(entries));
    refreshVersionsFromStore(rowStore, versions);
    indexes.rebuildIndexes(rowStore, schema);
}

PageStoreSnapshot TableSnapshotIO::exportPageStore(const RowStore &rowStore) const {
    const auto *pageStore = dynamic_cast<const PageRowStore *>(&rowStore);
    if (pageStore == nullptr) {
        throw std::runtime_error("table row store does not support page payload export");
    }
    return pageStore->exportPages();
}

void TableSnapshotIO::replaceFromPages(RowStore &rowStore, IndexManager &indexes,
                                       MVCCRowStore &versions, std::span<const Column> schema,
                                       PageStoreSnapshot snapshot, bool rebuildIndexesAfter) {
    auto *pageStore = dynamic_cast<PageRowStore *>(&rowStore);
    if (pageStore == nullptr) {
        throw std::runtime_error("table row store does not support page payload restore");
    }
    pageStore->replaceFromPages(std::move(snapshot));
    refreshVersionsFromStore(rowStore, versions);
    if (rebuildIndexesAfter) {
        indexes.rebuildIndexes(rowStore, schema);
    }
}

TableIndexStoreSnapshot
TableSnapshotIO::exportIndexPages(const IndexManager &indexes,
                                  std::span<const Column> schema) const {
    TableIndexStoreSnapshot snapshot;
    const auto &indexColumns = indexes.indexColumns();
    const auto &indexExpressions = indexes.indexExpressions();
    const auto &orderedIndexes = indexes.orderedIndexes();
    const auto &hashIndexes = indexes.hashIndexes();
    snapshot.indexes.reserve(indexColumns.size());
    for (const auto &[name, columnIndex] : indexColumns) {
        IndexStoreSnapshot entry;
        entry.name = name;
        if (auto it = indexExpressions.find(name); it != indexExpressions.end()) {
            entry.column = encodeIndexDefinitionColumn(schema[columnIndex].name, it->second);
        } else {
            entry.column = schema[columnIndex].name;
        }
        entry.btree = orderedIndexes.at(name).exportPages();
        entry.hash = hashIndexes.at(name).exportBuckets();
        snapshot.indexes.push_back(std::move(entry));
    }
    return snapshot;
}

void TableSnapshotIO::replaceIndexPages(IndexManager &indexes,
                                        TableIndexStoreSnapshot snapshot) {
    auto &hashIndexes = indexes.hashIndexes();
    auto &orderedIndexes = indexes.orderedIndexes();
    for (auto &entry : snapshot.indexes) {
        auto hashIt = hashIndexes.find(entry.name);
        auto btreeIt = orderedIndexes.find(entry.name);
        if (hashIt == hashIndexes.end() || btreeIt == orderedIndexes.end()) {
            throw std::runtime_error("index page restore references unknown index " + entry.name);
        }
        btreeIt->second.replaceFromPages(std::move(entry.btree));
        hashIt->second.replaceFromBuckets(std::move(entry.hash));
    }
}

void TableSnapshotIO::clearDirtyTracking(RowStore &rowStore, IndexManager &indexes) {
    if (auto *pageStore = dynamic_cast<PageRowStore *>(&rowStore); pageStore != nullptr) {
        pageStore->clearDirtyPages();
    }
    for (auto &[_, index] : indexes.hashIndexes()) {
        index.clearDirtyPages();
    }
    for (auto &[_, index] : indexes.orderedIndexes()) {
        index.clearDirtyPages();
    }
}

PageImageCapture TableSnapshotIO::takePageImageCapture(RowStore &rowStore,
                                                       IndexManager &indexes) {
    PageImageCapture capture;
    capture.capacity = rowStore.capacity();
    capture.freeList = rowStore.freeList();
    auto *pageStore = dynamic_cast<PageRowStore *>(&rowStore);
    if (pageStore == nullptr) {
        throw std::runtime_error("table row store does not support page image capture");
    }
    capture.heapPages = pageStore->takeDirtyPages();
    for (auto &[name, index] : indexes.orderedIndexes()) {
        if (index.hasDirtyPages()) {
            capture.btreeIndexes.emplace_back(name, index.takeDirtyPages());
        }
    }
    for (auto &[name, index] : indexes.hashIndexes()) {
        if (index.hasDirtyPages()) {
            capture.hashIndexes.emplace_back(name, index.takeDirtyBuckets());
        }
    }
    return capture;
}

void TableSnapshotIO::refreshVersionsFromStore(const RowStore &rowStore,
                                               MVCCRowStore &versions) {
    versions.clear();
    for (RowId rowId = 0; rowId < rowStore.capacity(); ++rowId) {
        const auto *row = rowStore.get(rowId);
        if (row == nullptr) {
            continue;
        }
        versions.write(rowId, *row, kSystemTransactionId);
    }
}

} // namespace VertexDB
