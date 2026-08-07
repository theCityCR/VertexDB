#include "VertexDB/storage/table.hpp"

#include "VertexDB/common/index_expression.hpp"

#include <algorithm>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>

namespace VertexDB {

void Table::analyze(std::size_t maxBuckets) {
    std::unique_lock lock{mutex_};
    histograms_.clear();
    const auto entries = rowStore_->liveEntries();
    for (std::size_t columnIndex = 0; columnIndex < schema_.size(); ++columnIndex) {
        std::vector<Value> values;
        values.reserve(entries.size());
        for (const auto &[rowId, row] : entries) {
            (void)rowId;
            if (columnIndex < row.size() && !row[columnIndex].isNull()) {
                values.push_back(row[columnIndex]);
            }
        }
        std::sort(values.begin(), values.end());
        auto histogram =
            buildEquiHeightHistogram(schema_[columnIndex].name, std::move(values), maxBuckets);
        histograms_.emplace(histogram.column, std::move(histogram));
    }
}

std::optional<ColumnHistogram> Table::columnHistogram(std::string_view column) const {
    std::shared_lock lock{mutex_};
    auto it = histograms_.find(std::string{column});
    if (it == histograms_.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::vector<ColumnHistogram> Table::columnHistograms() const {
    std::shared_lock lock{mutex_};
    std::vector<ColumnHistogram> out;
    out.reserve(histograms_.size());
    for (const auto &[_, histogram] : histograms_) {
        out.push_back(histogram);
    }
    return out;
}

void Table::replaceColumnHistograms(std::vector<ColumnHistogram> histograms) {
    std::unique_lock lock{mutex_};
    histograms_.clear();
    for (auto &histogram : histograms) {
        histograms_.emplace(histogram.column, std::move(histogram));
    }
}

void Table::clearColumnHistograms() {
    std::unique_lock lock{mutex_};
    histograms_.clear();
}

bool Table::applyPhysicalUpsert(RowId rowId, Row row) {
    validateRow(row);
    std::unique_lock lock{mutex_};
    const bool existed = rowStore_->get(rowId) != nullptr;
    if (!rowStore_->upsertAt(rowId, std::move(row))) {
        return false;
    }
    versions_.write(rowId, *rowStore_->get(rowId), kSystemTransactionId);
    if (existed) {
        rebuildIndexes();
    } else {
        addRowToIndexes(rowId);
    }
    return true;
}

bool Table::applyPhysicalErase(RowId rowId) {
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

void Table::applyPageImageRedo(
    bool hasHeapMeta, std::size_t capacity, std::vector<RowId> freeList,
    std::vector<std::pair<PageId, std::vector<std::byte>>> heapPages,
    std::vector<std::pair<std::string, BTreeIndexSnapshot>> btreeIndexes,
    std::vector<std::pair<std::string, HashIndexSnapshot>> hashIndexes) {
    std::unique_lock lock{mutex_};
    auto *pageStore = dynamic_cast<PageRowStore *>(rowStore_.get());
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
    for (auto &[name, snapshot] : btreeIndexes) {
        auto it = orderedIndexes_.find(name);
        if (it == orderedIndexes_.end()) {
            throw std::runtime_error("page image redo references unknown btree index " + name);
        }
        it->second.applyDirtyPages(snapshot);
    }
    for (auto &[name, snapshot] : hashIndexes) {
        auto it = indexes_.find(name);
        if (it == indexes_.end()) {
            throw std::runtime_error("page image redo references unknown hash index " + name);
        }
        it->second.applyDirtyBuckets(snapshot);
    }
    refreshVersionsFromStore();
}

void Table::replaceRows(std::vector<Row> rows) {
    for (const auto &row : rows) {
        validateRow(row);
    }
    std::unique_lock lock{mutex_};
    rowStore_->replaceRows(std::move(rows));
    versions_.clear();
    for (RowId rowId = 0; rowId < rowStore_->capacity(); ++rowId) {
        const auto *row = rowStore_->get(rowId);
        if (row == nullptr) {
            continue;
        }
        versions_.write(rowId, *row, kSystemTransactionId);
    }
    rebuildIndexes();
}

void Table::replaceSparse(std::size_t capacity, std::vector<RowId> freeList,
                          std::vector<std::pair<RowId, Row>> entries) {
    for (const auto &[_, row] : entries) {
        validateRow(row);
    }
    std::unique_lock lock{mutex_};
    rowStore_->replaceSparse(capacity, std::move(freeList), std::move(entries));
    versions_.clear();
    for (RowId rowId = 0; rowId < rowStore_->capacity(); ++rowId) {
        const auto *row = rowStore_->get(rowId);
        if (row == nullptr) {
            continue;
        }
        versions_.write(rowId, *row, kSystemTransactionId);
    }
    rebuildIndexes();
}

PageStoreSnapshot Table::exportPageStore() const {
    std::shared_lock lock{mutex_};
    const auto *pageStore = dynamic_cast<const PageRowStore *>(rowStore_.get());
    if (pageStore == nullptr) {
        throw std::runtime_error("table row store does not support page payload export");
    }
    return pageStore->exportPages();
}

void Table::replaceFromPages(PageStoreSnapshot snapshot) {
    replaceFromPages(std::move(snapshot), true);
}

void Table::replaceFromPages(PageStoreSnapshot snapshot, bool rebuildIndexesAfter) {
    validatePageStoreLayout(snapshot);
    for (const auto &[_, bytes] : snapshot.pages) {
        for (const auto &row : PageRowStore::decodePage(bytes)) {
            if (!row.empty()) {
                validateRow(row);
            }
        }
    }

    std::unique_lock lock{mutex_};
    auto *pageStore = dynamic_cast<PageRowStore *>(rowStore_.get());
    if (pageStore == nullptr) {
        throw std::runtime_error("table row store does not support page payload restore");
    }
    pageStore->replaceFromPages(std::move(snapshot));
    refreshVersionsFromStore();
    if (rebuildIndexesAfter) {
        rebuildIndexes();
    }
}

TableIndexStoreSnapshot Table::exportIndexPages() const {
    std::shared_lock lock{mutex_};
    TableIndexStoreSnapshot snapshot;
    snapshot.indexes.reserve(indexColumns_.size());
    for (const auto &[name, columnIndex] : indexColumns_) {
        IndexStoreSnapshot entry;
        entry.name = name;
        if (auto it = indexExpressions_.find(name); it != indexExpressions_.end()) {
            entry.column = encodeIndexDefinitionColumn(schema_.at(columnIndex).name, it->second);
        } else {
            entry.column = schema_.at(columnIndex).name;
        }
        entry.btree = orderedIndexes_.at(name).exportPages();
        entry.hash = indexes_.at(name).exportBuckets();
        snapshot.indexes.push_back(std::move(entry));
    }
    return snapshot;
}

void Table::replaceIndexPages(TableIndexStoreSnapshot snapshot) {
    std::unique_lock lock{mutex_};
    for (auto &entry : snapshot.indexes) {
        auto hashIt = indexes_.find(entry.name);
        auto btreeIt = orderedIndexes_.find(entry.name);
        if (hashIt == indexes_.end() || btreeIt == orderedIndexes_.end()) {
            throw std::runtime_error("index page restore references unknown index " + entry.name);
        }
        btreeIt->second.replaceFromPages(std::move(entry.btree));
        hashIt->second.replaceFromBuckets(std::move(entry.hash));
    }
}

void Table::clearDirtyTracking() {
    std::unique_lock lock{mutex_};
    if (auto *pageStore = dynamic_cast<PageRowStore *>(rowStore_.get()); pageStore != nullptr) {
        pageStore->clearDirtyPages();
    }
    for (auto &[_, index] : indexes_) {
        index.clearDirtyPages();
    }
    for (auto &[_, index] : orderedIndexes_) {
        index.clearDirtyPages();
    }
}

PageImageCapture Table::takePageImageCapture() {
    std::unique_lock lock{mutex_};
    PageImageCapture capture;
    capture.capacity = rowStore_->capacity();
    capture.freeList = rowStore_->freeList();
    auto *pageStore = dynamic_cast<PageRowStore *>(rowStore_.get());
    if (pageStore == nullptr) {
        throw std::runtime_error("table row store does not support page image capture");
    }
    capture.heapPages = pageStore->takeDirtyPages();
    for (auto &[name, index] : orderedIndexes_) {
        if (index.hasDirtyPages()) {
            capture.btreeIndexes.emplace_back(name, index.takeDirtyPages());
        }
    }
    for (auto &[name, index] : indexes_) {
        if (index.hasDirtyPages()) {
            capture.hashIndexes.emplace_back(name, index.takeDirtyBuckets());
        }
    }
    return capture;
}

void Table::refreshVersionsFromStore() {
    versions_.clear();
    for (RowId rowId = 0; rowId < rowStore_->capacity(); ++rowId) {
        const auto *row = rowStore_->get(rowId);
        if (row == nullptr) {
            continue;
        }
        versions_.write(rowId, *row, kSystemTransactionId);
    }
}

} // namespace VertexDB
