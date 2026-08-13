#include "VertexDB/storage/table.hpp"

#include <mutex>
#include <utility>

namespace VertexDB {

bool Table::eraseDiscardingVersion(RowId rowId) {
    std::unique_lock lock{mutex_};
    if (rowStore_->get(rowId) == nullptr) {
        return false;
    }
    if (!rowStore_->erase(rowId)) {
        return false;
    }
    (void)versions_.popLatestVersion(rowId);
    rebuildIndexes();
    return true;
}

bool Table::replaceRow(RowId rowId, Row row) {
    validateRow(row);
    std::unique_lock lock{mutex_};
    if (rowStore_->get(rowId) == nullptr) {
        return false;
    }
    enforceUniqueConstraintsUnlocked(row, rowId);
    if (!rowStore_->update(rowId, std::move(row))) {
        return false;
    }
    (void)versions_.popLatestVersion(rowId);
    rebuildIndexes();
    return true;
}

bool Table::revive(RowId rowId, Row row) {
    validateRow(row);
    std::unique_lock lock{mutex_};
    enforceUniqueConstraintsUnlocked(row, rowId);
    if (!rowStore_->revive(rowId, std::move(row))) {
        return false;
    }
    (void)versions_.clearLatestDeletedBy(rowId);
    rebuildIndexes();
    return true;
}

bool Table::applyPhysicalUpsert(RowId rowId, Row row) {
    validateRow(row);
    std::unique_lock lock{mutex_};
    return snapshotIo_.applyPhysicalUpsert(*rowStore_, indexManager_, versions_, schema_, rowId,
                                           std::move(row));
}

bool Table::applyPhysicalErase(RowId rowId) {
    std::unique_lock lock{mutex_};
    return snapshotIo_.applyPhysicalErase(*rowStore_, indexManager_, versions_, schema_, rowId);
}

void Table::applyPageImageRedo(
    bool hasHeapMeta, std::size_t capacity, std::vector<RowId> freeList,
    std::vector<std::pair<PageId, std::vector<std::byte>>> heapPages,
    std::vector<std::pair<std::string, BTreeIndexSnapshot>> btreeIndexes,
    std::vector<std::pair<std::string, HashIndexSnapshot>> hashIndexes) {
    std::unique_lock lock{mutex_};
    snapshotIo_.applyPageImageRedo(*rowStore_, indexManager_, versions_, hasHeapMeta, capacity,
                                   std::move(freeList), std::move(heapPages),
                                   std::move(btreeIndexes), std::move(hashIndexes));
}

} // namespace VertexDB
