#pragma once

#include "VertexDB/storage/buffer_pool.hpp"
#include "VertexDB/storage/row.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
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

// Durable page-directory snapshot for PageRowStore (on-disk format v3).
struct PageStoreSnapshot {
    std::size_t rowsPerPage{0};
    std::size_t capacity{0};
    std::vector<RowId> freeList;
    std::vector<std::pair<PageId, std::vector<std::byte>>> pages; // ascending PageId
};

void validatePageStoreLayout(const PageStoreSnapshot &snapshot);

class VectorRowStore final : public RowStore {
  public:
    [[nodiscard]] RowId append(Row row) override;
    [[nodiscard]] bool erase(RowId rowId) override;
    [[nodiscard]] bool update(RowId rowId, Row row) override;
    [[nodiscard]] bool revive(RowId rowId, Row row) override;
    [[nodiscard]] bool upsertAt(RowId rowId, Row row) override;
    [[nodiscard]] const Row *get(RowId rowId) const override;
    [[nodiscard]] std::vector<Row> snapshot() const override;
    [[nodiscard]] std::vector<std::pair<RowId, Row>> liveEntries() const override;
    [[nodiscard]] std::vector<RowId> freeList() const override;
    [[nodiscard]] std::vector<Row> rowsById(std::span<const RowId> rowIds) const override;
    [[nodiscard]] std::size_t size() const noexcept override;
    [[nodiscard]] std::size_t capacity() const noexcept override;
    void replaceRows(std::vector<Row> rows) override;
    void replaceSparse(std::size_t capacity, std::vector<RowId> freeList,
                       std::vector<std::pair<RowId, Row>> entries) override;

  private:
    std::vector<std::optional<Row>> rows_;
    std::vector<RowId> freeList_;
    std::size_t liveCount_{0};
};

class PageRowStore final : public RowStore {
  public:
    explicit PageRowStore(std::size_t rowsPerPage = 256, std::size_t bufferPageCapacity = 128);

    [[nodiscard]] RowId append(Row row) override;
    [[nodiscard]] bool erase(RowId rowId) override;
    [[nodiscard]] bool update(RowId rowId, Row row) override;
    [[nodiscard]] bool revive(RowId rowId, Row row) override;
    [[nodiscard]] bool upsertAt(RowId rowId, Row row) override;
    [[nodiscard]] const Row *get(RowId rowId) const override;
    [[nodiscard]] std::vector<Row> snapshot() const override;
    [[nodiscard]] std::vector<std::pair<RowId, Row>> liveEntries() const override;
    [[nodiscard]] std::vector<RowId> freeList() const override;
    [[nodiscard]] std::vector<Row> rowsById(std::span<const RowId> rowIds) const override;
    [[nodiscard]] std::size_t size() const noexcept override;
    [[nodiscard]] std::size_t capacity() const noexcept override;
    // Observability: page-byte directory is SoT; buffer pool is the LRU access cache.
    [[nodiscard]] bool bufferContains(PageId pageId) const;
    [[nodiscard]] std::size_t bufferSize() const noexcept;
    [[nodiscard]] PageId pageIdFor(RowId rowId) const;
    [[nodiscard]] std::optional<std::vector<std::byte>> directoryBytes(PageId pageId) const;
    [[nodiscard]] static std::vector<Row> decodePage(std::span<const std::byte> bytes);
    [[nodiscard]] std::size_t rowsPerPage() const noexcept;
    [[nodiscard]] PageStoreSnapshot exportPages() const;
    void replaceFromPages(PageStoreSnapshot snapshot);
    void replaceRows(std::vector<Row> rows) override;
    void replaceSparse(std::size_t capacity, std::vector<RowId> freeList,
                       std::vector<std::pair<RowId, Row>> entries) override;

  private:
    struct Slot {
        PageId pageId{};
        std::size_t offset{};
        bool live{false};
    };

    [[nodiscard]] Page serializePage(PageId pageId, const std::vector<Row> &rows) const;
    [[nodiscard]] std::vector<Row> loadPageRows(PageId pageId) const;
    void storePage(PageId pageId, const std::vector<Row> &rows);
    void ensureBuffered(PageId pageId) const;
    void invalidateDecoded(PageId pageId);

    std::size_t rowsPerPage_;
    mutable BufferPool bufferPool_;
    std::vector<Slot> slots_;
    std::vector<RowId> freeList_;
    std::unordered_map<PageId, std::vector<std::byte>> pageDirectory_;
    mutable std::unordered_map<RowId, Row> decodedRows_;
    std::size_t liveCount_{0};
};

[[nodiscard]] std::unique_ptr<RowStore> makeVectorRowStore();
[[nodiscard]] std::unique_ptr<RowStore> makePageRowStore();

} // namespace VertexDB
