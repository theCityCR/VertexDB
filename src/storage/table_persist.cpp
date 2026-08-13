#include "VertexDB/storage/table.hpp"

#include "VertexDB/storage/page_row_store.hpp"

#include <mutex>
#include <shared_mutex>

namespace VertexDB {

void Table::analyze(std::size_t maxBuckets) {
    std::unique_lock lock{mutex_};
    statistics_.analyze(schema_, *rowStore_, maxBuckets);
}

std::optional<ColumnHistogram> Table::columnHistogram(std::string_view column) const {
    std::shared_lock lock{mutex_};
    return statistics_.columnHistogram(column);
}

std::vector<ColumnHistogram> Table::columnHistograms() const {
    std::shared_lock lock{mutex_};
    return statistics_.columnHistograms();
}

void Table::replaceColumnHistograms(std::vector<ColumnHistogram> histograms) {
    std::unique_lock lock{mutex_};
    statistics_.replaceColumnHistograms(std::move(histograms));
}

void Table::clearColumnHistograms() {
    std::unique_lock lock{mutex_};
    statistics_.clearColumnHistograms();
}

void Table::replaceRows(std::vector<Row> rows) {
    for (const auto &row : rows) {
        validateRow(row);
    }
    std::unique_lock lock{mutex_};
    snapshotIo_.replaceRows(*rowStore_, indexManager_, versions_, schema_, std::move(rows));
}

void Table::replaceSparse(std::size_t capacity, std::vector<RowId> freeList,
                          std::vector<std::pair<RowId, Row>> entries) {
    for (const auto &[_, row] : entries) {
        validateRow(row);
    }
    std::unique_lock lock{mutex_};
    snapshotIo_.replaceSparse(*rowStore_, indexManager_, versions_, schema_, capacity,
                              std::move(freeList), std::move(entries));
}

PageStoreSnapshot Table::exportPageStore() const {
    std::shared_lock lock{mutex_};
    return snapshotIo_.exportPageStore(*rowStore_);
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
    snapshotIo_.replaceFromPages(*rowStore_, indexManager_, versions_, schema_, std::move(snapshot),
                                 rebuildIndexesAfter);
}

TableIndexStoreSnapshot Table::exportIndexPages() const {
    std::shared_lock lock{mutex_};
    return snapshotIo_.exportIndexPages(indexManager_, schema_);
}

void Table::replaceIndexPages(TableIndexStoreSnapshot snapshot) {
    std::unique_lock lock{mutex_};
    snapshotIo_.replaceIndexPages(indexManager_, std::move(snapshot));
}

void Table::clearDirtyTracking() {
    std::unique_lock lock{mutex_};
    snapshotIo_.clearDirtyTracking(*rowStore_, indexManager_);
}

PageImageCapture Table::takePageImageCapture() {
    std::unique_lock lock{mutex_};
    return snapshotIo_.takePageImageCapture(*rowStore_, indexManager_);
}

} // namespace VertexDB
