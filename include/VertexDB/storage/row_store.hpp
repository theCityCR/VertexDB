#pragma once

#include "VertexDB/storage/buffer_pool.hpp"
#include "VertexDB/storage/row.hpp"

#include <cstddef>
#include <memory>
#include <span>
#include <unordered_map>
#include <vector>

namespace VertexDB {

class RowStore {
  public:
    virtual ~RowStore() = default;

    [[nodiscard]] virtual RowId append(Row row) = 0;
    [[nodiscard]] virtual bool erase(RowId rowId) = 0;
    [[nodiscard]] virtual bool update(RowId rowId, Row row) = 0;
    [[nodiscard]] virtual const Row *get(RowId rowId) const = 0;
    [[nodiscard]] virtual std::vector<Row> snapshot() const = 0;
    [[nodiscard]] virtual std::vector<Row> rowsById(std::span<const RowId> rowIds) const = 0;
    [[nodiscard]] virtual std::size_t size() const noexcept = 0;
    virtual void replaceRows(std::vector<Row> rows) = 0;
};

class VectorRowStore final : public RowStore {
  public:
    [[nodiscard]] RowId append(Row row) override;
    [[nodiscard]] bool erase(RowId rowId) override;
    [[nodiscard]] bool update(RowId rowId, Row row) override;
    [[nodiscard]] const Row *get(RowId rowId) const override;
    [[nodiscard]] std::vector<Row> snapshot() const override;
    [[nodiscard]] std::vector<Row> rowsById(std::span<const RowId> rowIds) const override;
    [[nodiscard]] std::size_t size() const noexcept override;
    void replaceRows(std::vector<Row> rows) override;

  private:
    std::vector<Row> rows_;
};

class PageRowStore final : public RowStore {
  public:
    explicit PageRowStore(std::size_t rowsPerPage = 256, std::size_t bufferPageCapacity = 128);

    [[nodiscard]] RowId append(Row row) override;
    [[nodiscard]] bool erase(RowId rowId) override;
    [[nodiscard]] bool update(RowId rowId, Row row) override;
    [[nodiscard]] const Row *get(RowId rowId) const override;
    [[nodiscard]] std::vector<Row> snapshot() const override;
    [[nodiscard]] std::vector<Row> rowsById(std::span<const RowId> rowIds) const override;
    [[nodiscard]] std::size_t size() const noexcept override;
    void replaceRows(std::vector<Row> rows) override;

  private:
    struct Slot {
        PageId pageId{};
        std::size_t offset{};
    };

    [[nodiscard]] Page serializePage(PageId pageId, const std::vector<Row> &rows) const;

    std::size_t rowsPerPage_;
    BufferPool bufferPool_;
    std::vector<Slot> slots_;
    std::unordered_map<PageId, std::vector<Row>> pages_;
};

[[nodiscard]] std::unique_ptr<RowStore> makeVectorRowStore();
[[nodiscard]] std::unique_ptr<RowStore> makePageRowStore();

} // namespace VertexDB
