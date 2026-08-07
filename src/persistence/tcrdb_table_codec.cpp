#include "tcrdb_detail.hpp"

#include <cstddef>
#include <utility>
#include <vector>

namespace VertexDB::tcrdb_detail {

void loadDenseRows(Table &table, std::istream &in, std::size_t columnCount) {
    const auto rowCount = readPodDb<std::uint64_t>(in);
    std::vector<Row> rows;
    rows.reserve(static_cast<std::size_t>(rowCount));
    for (std::uint64_t rowIndex = 0; rowIndex < rowCount; ++rowIndex) {
        rows.push_back(readRow(in, columnCount));
    }
    table.replaceRows(std::move(rows));
}

void loadSparseRows(Table &table, std::istream &in, std::size_t columnCount) {
    const auto capacity = static_cast<std::size_t>(readPodDb<std::uint64_t>(in));
    const auto freeCount = readPodDb<std::uint64_t>(in);
    std::vector<RowId> freeList;
    freeList.reserve(static_cast<std::size_t>(freeCount));
    for (std::uint64_t index = 0; index < freeCount; ++index) {
        freeList.push_back(static_cast<RowId>(readPodDb<std::uint64_t>(in)));
    }

    const auto liveCount = readPodDb<std::uint64_t>(in);
    std::vector<std::pair<RowId, Row>> entries;
    entries.reserve(static_cast<std::size_t>(liveCount));
    for (std::uint64_t index = 0; index < liveCount; ++index) {
        const auto rowId = static_cast<RowId>(readPodDb<std::uint64_t>(in));
        entries.emplace_back(rowId, readRow(in, columnCount));
    }
    table.replaceSparse(capacity, std::move(freeList), std::move(entries));
}

void loadPagePayloadRows(Table &table, std::istream &in, bool rebuildIndexes) {
    PageStoreSnapshot snapshot;
    snapshot.rowsPerPage = static_cast<std::size_t>(readPodDb<std::uint64_t>(in));
    snapshot.capacity = static_cast<std::size_t>(readPodDb<std::uint64_t>(in));

    const auto freeCount = readPodDb<std::uint64_t>(in);
    snapshot.freeList.reserve(static_cast<std::size_t>(freeCount));
    for (std::uint64_t index = 0; index < freeCount; ++index) {
        snapshot.freeList.push_back(static_cast<RowId>(readPodDb<std::uint64_t>(in)));
    }

    const auto pageCount = readPodDb<std::uint64_t>(in);
    snapshot.pages.reserve(static_cast<std::size_t>(pageCount));
    for (std::uint64_t index = 0; index < pageCount; ++index) {
        const auto pageId = static_cast<PageId>(readPodDb<std::uint64_t>(in));
        const auto byteLength = static_cast<std::size_t>(readPodDb<std::uint64_t>(in));
        std::vector<std::byte> bytes(byteLength);
        if (byteLength > 0) {
            readBytesDb(in, bytes.data(), bytes.size());
        }
        snapshot.pages.emplace_back(pageId, std::move(bytes));
    }
    table.replaceFromPages(std::move(snapshot), rebuildIndexes);
}

void writePagePayloadRows(std::ostream &out, const Table &table) {
    const auto snapshot = table.exportPageStore();
    writePodDb(out, static_cast<std::uint64_t>(snapshot.rowsPerPage));
    writePodDb(out, static_cast<std::uint64_t>(snapshot.capacity));
    writePodDb(out, static_cast<std::uint64_t>(snapshot.freeList.size()));
    for (const auto rowId : snapshot.freeList) {
        writePodDb(out, static_cast<std::uint64_t>(rowId));
    }
    writePodDb(out, static_cast<std::uint64_t>(snapshot.pages.size()));
    for (const auto &[pageId, bytes] : snapshot.pages) {
        writePodDb(out, static_cast<std::uint64_t>(pageId));
        writePodDb(out, static_cast<std::uint64_t>(bytes.size()));
        if (!bytes.empty()) {
            writeBytesDb(out, bytes.data(), bytes.size());
        }
    }
}

} // namespace VertexDB::tcrdb_detail
