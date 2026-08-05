#pragma once

#include "VertexDB/common/comparison_operator.hpp"
#include "VertexDB/common/value.hpp"
#include "VertexDB/indexing/btree_index.hpp"
#include "VertexDB/indexing/hash_index.hpp"
#include "VertexDB/storage/row.hpp"
#include "VertexDB/storage/row_store.hpp"
#include "VertexDB/transaction/mvcc_row_store.hpp"

#include <map>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace VertexDB {

class Table {
  public:
    Table(std::string name, std::vector<Column> schema);

    [[nodiscard]] const std::string &name() const noexcept;
    [[nodiscard]] std::span<const Column> schema() const noexcept;
    [[nodiscard]] std::optional<std::size_t> columnIndex(std::string_view column) const;
    [[nodiscard]] std::vector<Row> rowsSnapshot() const;
    [[nodiscard]] std::vector<Row> rowsSnapshot(TransactionId readerId) const;
    [[nodiscard]] std::vector<Row> rowsById(std::span<const RowId> rowIds) const;
    [[nodiscard]] std::vector<Row> rowsById(std::span<const RowId> rowIds,
                                            TransactionId readerId) const;
    [[nodiscard]] std::size_t rowCount() const;
    [[nodiscard]] std::vector<RowId> findIndexed(std::string_view column, const Value &value) const;
    [[nodiscard]] std::optional<std::vector<RowId>> indexedLookup(std::string_view column,
                                                                  const Value &value) const;
    [[nodiscard]] std::optional<std::vector<RowId>>
    orderedLookup(std::string_view column, ComparisonOperator op, const Value &value) const;
    [[nodiscard]] bool hasIndex(std::string_view column) const;
    [[nodiscard]] bool hasOrderedIndex(std::string_view column) const;
    [[nodiscard]] std::vector<std::string> listIndexes() const;
    [[nodiscard]] std::vector<std::pair<std::string, std::string>> indexDefinitions() const;
    [[nodiscard]] std::size_t versionCount(RowId rowId) const;
    void validateRow(const Row &row) const;

    RowId insert(Row row);
    bool erase(RowId rowId);
    bool update(RowId rowId, std::size_t columnIndex, Value value);
    bool createIndex(std::string name, std::string column);
    void replaceRows(std::vector<Row> rows);

  private:
    void addRowToIndexes(RowId rowId);
    void rebuildIndexes();

    std::string name_;
    std::vector<Column> schema_;
    std::unique_ptr<RowStore> rowStore_;
    std::map<std::string, std::size_t> indexColumns_;
    std::map<std::string, HashIndex> indexes_;
    std::map<std::string, BTreeIndex> orderedIndexes_;
    MVCCRowStore versions_;
    TransactionId nextVersionTransactionId_{1};
    mutable std::shared_mutex mutex_;
};

} // namespace VertexDB
