#pragma once

// In-memory vector-backed RowStore (tests / comparisons). Implementation: vector_row_store.cpp.

#include "VertexDB/storage/row_store.hpp"

#include <memory>
#include <optional>
#include <vector>

namespace VertexDB {

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

[[nodiscard]] std::unique_ptr<RowStore> makeVectorRowStore();

} // namespace VertexDB
