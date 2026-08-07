#pragma once

// Index definitions plus hash/B+ tree stores; maintenance against RowStore + schema.
// Table holds the mutex and forwards its public index API while locked.

#include "VertexDB/common/comparison_operator.hpp"
#include "VertexDB/common/index_expression.hpp"
#include "VertexDB/indexing/btree_index.hpp"
#include "VertexDB/indexing/hash_index.hpp"
#include "VertexDB/storage/row.hpp"
#include "VertexDB/storage/row_store.hpp"

#include <cstddef>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace VertexDB {

struct IndexDefinition {
    std::string name;
    std::string column;
    std::optional<IndexExpression> expression;
};

// Owns index metadata and stores. Its owning Table provides synchronization.
class IndexManager {
  public:
    [[nodiscard]] std::optional<std::size_t>
    indexDistinctCount(std::string_view column, std::span<const Column> schema) const;
    [[nodiscard]] std::optional<std::size_t>
    indexDistinctCount(const IndexExpression &expression) const;
    [[nodiscard]] std::optional<std::vector<RowId>>
    indexedLookup(std::string_view column, const Value &value,
                  std::span<const Column> schema) const;
    [[nodiscard]] std::optional<std::vector<RowId>>
    indexedLookup(const IndexExpression &expression, const Value &value) const;
    [[nodiscard]] std::optional<std::vector<RowId>>
    orderedLookup(std::string_view column, ComparisonOperator op, const Value &value,
                  std::span<const Column> schema) const;
    [[nodiscard]] std::optional<std::vector<RowId>>
    orderedLookup(const IndexExpression &expression, ComparisonOperator op,
                  const Value &value) const;

    [[nodiscard]] bool hasIndex(std::string_view column, std::span<const Column> schema) const;
    [[nodiscard]] bool hasExpressionIndex(const IndexExpression &expression) const;
    [[nodiscard]] std::vector<std::string> listIndexes() const;
    [[nodiscard]] std::vector<IndexDefinition>
    indexDefinitions(std::span<const Column> schema) const;
    [[nodiscard]] std::optional<std::vector<BTreeNode>>
    orderedIndexNodesSnapshot(std::string_view indexName) const;

    bool registerIndex(std::string name, std::size_t columnIndex,
                       std::optional<IndexExpression> expression, bool rebuild,
                       const RowStore &rowStore, std::span<const Column> schema);
    void addRowToIndexes(RowId rowId, const RowStore &rowStore,
                         std::span<const Column> schema);
    void rebuildIndexes(const RowStore &rowStore, std::span<const Column> schema);

    [[nodiscard]] const std::map<std::string, std::size_t> &indexColumns() const noexcept;
    [[nodiscard]] std::map<std::string, std::size_t> &indexColumns() noexcept;
    [[nodiscard]] const std::map<std::string, IndexExpression> &indexExpressions() const noexcept;
    [[nodiscard]] std::map<std::string, IndexExpression> &indexExpressions() noexcept;
    [[nodiscard]] const std::map<std::string, HashIndex> &hashIndexes() const noexcept;
    [[nodiscard]] std::map<std::string, HashIndex> &hashIndexes() noexcept;
    [[nodiscard]] const std::map<std::string, BTreeIndex> &orderedIndexes() const noexcept;
    [[nodiscard]] std::map<std::string, BTreeIndex> &orderedIndexes() noexcept;

  private:
    [[nodiscard]] Value indexKeyForRow(const std::string &indexName, const Row &row,
                                       std::span<const Column> schema) const;

    std::map<std::string, std::size_t> indexColumns_;
    std::map<std::string, IndexExpression> indexExpressions_;
    std::map<std::string, HashIndex> indexes_;
    std::map<std::string, BTreeIndex> orderedIndexes_;
};

} // namespace VertexDB
