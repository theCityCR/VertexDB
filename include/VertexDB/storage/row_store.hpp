#pragma once

// Abstract RowStore plus shared sparse/page-layout validators.
// Concrete stores: page_row_store.hpp, vector_row_store.hpp. See table.hpp for ownership.

#include "VertexDB/storage/buffer_pool.hpp"
#include "VertexDB/storage/row.hpp"

#include <cstddef>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace VertexDB {

class RowStore {
  public:
    virtual ~RowStore() = default;

    [[nodiscard]] virtual RowId append(Row row) = 0;
    [[nodiscard]] virtual bool erase(RowId rowId) = 0;
    [[nodiscard]] virtual bool update(RowId rowId, Row row) = 0;
    // Restore a free/tombstoned row id with the given payload (undo of erase).
    [[nodiscard]] virtual bool revive(RowId rowId, Row row) = 0;
    // Place an after-image at an explicit row id for physical redo replay. Extends capacity only
    // when rowId == capacity() (does not consume the free list for end-extension).
    [[nodiscard]] virtual bool upsertAt(RowId rowId, Row row) = 0;
    [[nodiscard]] virtual const Row *get(RowId rowId) const = 0;
    [[nodiscard]] virtual std::vector<Row> snapshot() const = 0;
    [[nodiscard]] virtual std::vector<std::pair<RowId, Row>> liveEntries() const = 0;
    [[nodiscard]] virtual std::vector<RowId> freeList() const = 0;
    [[nodiscard]] virtual std::vector<Row> rowsById(std::span<const RowId> rowIds) const = 0;
    [[nodiscard]] virtual std::size_t size() const noexcept = 0;
    [[nodiscard]] virtual std::size_t capacity() const noexcept = 0;
    virtual void replaceRows(std::vector<Row> rows) = 0;
    virtual void replaceSparse(std::size_t capacity, std::vector<RowId> freeList,
                               std::vector<std::pair<RowId, Row>> entries) = 0;
};

// Shared by VectorRowStore and PageRowStore replaceSparse implementations.
void validateSparseRowLayout(std::size_t capacity, const std::vector<RowId> &freeList,
                             const std::vector<std::pair<RowId, Row>> &entries);

// Durable page-directory snapshot for PageRowStore (on-disk format v3+).
struct PageStoreSnapshot {
    std::size_t rowsPerPage{0};
    std::size_t capacity{0};
    std::vector<RowId> freeList;
    std::vector<std::pair<PageId, std::vector<std::byte>>> pages; // ascending PageId
};

void validatePageStoreLayout(const PageStoreSnapshot &snapshot);

} // namespace VertexDB
