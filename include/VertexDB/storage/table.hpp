#pragma once

#include "VertexDB/common/comparison_operator.hpp"
#include "VertexDB/common/value.hpp"
#include "VertexDB/indexing/btree_index.hpp"
#include "VertexDB/indexing/hash_index.hpp"
#include "VertexDB/parser/ast.hpp"
#include "VertexDB/storage/histogram.hpp"
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

struct IndexStoreSnapshot {
    std::string name;
    std::string column;
    BTreeIndexSnapshot btree;
    HashIndexSnapshot hash;
};

struct TableIndexStoreSnapshot {
    std::vector<IndexStoreSnapshot> indexes;
};

struct PageImageCapture {
    std::size_t capacity{};
    std::vector<RowId> freeList;
    std::vector<std::pair<PageId, std::vector<std::byte>>> heapPages;
    std::vector<std::pair<std::string, BTreeIndexSnapshot>> btreeIndexes;
    std::vector<std::pair<std::string, HashIndexSnapshot>> hashIndexes;
};

struct IndexDefinition {
    std::string name;
    std::string column;
    std::optional<IndexExpression> expression;
};

class Table {
  public:
    Table(std::string name, std::vector<Column> schema);

    [[nodiscard]] const std::string &name() const noexcept;
    [[nodiscard]] std::span<const Column> schema() const noexcept;
    [[nodiscard]] std::optional<std::size_t> columnIndex(std::string_view column) const;
    [[nodiscard]] std::vector<Row> rowsSnapshot() const;
    [[nodiscard]] std::vector<Row> rowsSnapshot(const ReadSnapshot &snapshot,
                                                const TransactionManager &transactions) const;
    [[nodiscard]] std::vector<std::pair<RowId, Row>> liveEntries() const;
    [[nodiscard]] std::vector<std::pair<RowId, Row>>
    visibleEntries(const ReadSnapshot &snapshot, const TransactionManager &transactions) const;
    [[nodiscard]] std::vector<RowId> freeList() const;
    [[nodiscard]] std::vector<Row> rowsById(std::span<const RowId> rowIds) const;
    [[nodiscard]] std::vector<Row> rowsById(std::span<const RowId> rowIds,
                                            const ReadSnapshot &snapshot,
                                            const TransactionManager &transactions) const;
    [[nodiscard]] std::size_t rowCount() const;
    [[nodiscard]] std::size_t capacity() const;
    // On-demand stats for cost-based planning (row count + per-index distinct keys).
    [[nodiscard]] std::optional<std::size_t> indexDistinctCount(std::string_view column) const;
    [[nodiscard]] std::optional<std::size_t>
    indexDistinctCount(const IndexExpression &expression) const;
    // ANALYZE builds equi-height per-column histograms (+ distinct counts) from live rows.
    void analyze(std::size_t maxBuckets = kDefaultHistogramBuckets);
    [[nodiscard]] std::optional<ColumnHistogram> columnHistogram(std::string_view column) const;
    [[nodiscard]] std::vector<ColumnHistogram> columnHistograms() const;
    void replaceColumnHistograms(std::vector<ColumnHistogram> histograms);
    void clearColumnHistograms();
    [[nodiscard]] std::optional<std::vector<RowId>> indexedLookup(std::string_view column,
                                                                  const Value &value) const;
    [[nodiscard]] std::optional<std::vector<RowId>>
    indexedLookup(const IndexExpression &expression, const Value &value) const;
    [[nodiscard]] std::optional<std::vector<RowId>>
    orderedLookup(std::string_view column, ComparisonOperator op, const Value &value) const;
    [[nodiscard]] std::optional<std::vector<RowId>>
    orderedLookup(const IndexExpression &expression, ComparisonOperator op,
                  const Value &value) const;
    // True when the column has a maintained column index (hash equality + ordered range).
    [[nodiscard]] bool hasIndex(std::string_view column) const;
    [[nodiscard]] bool hasExpressionIndex(const IndexExpression &expression) const;
    [[nodiscard]] std::vector<std::string> listIndexes() const;
    [[nodiscard]] std::vector<IndexDefinition> indexDefinitions() const;
    [[nodiscard]] std::size_t versionCount(RowId rowId) const;
    void validateRow(const Row &row) const;
    // Observability for tests: ordered index node layout for a named index.
    [[nodiscard]] std::optional<std::vector<BTreeNode>>
    orderedIndexNodesSnapshot(std::string_view indexName) const;

    RowId insert(Row row, TransactionId writerId = kSystemTransactionId);
    bool erase(RowId rowId, TransactionId writerId = kSystemTransactionId);
    bool update(RowId rowId, std::size_t columnIndex, Value value,
                TransactionId writerId = kSystemTransactionId);
    // Undo helpers: reverse INSERT/UPDATE/DELETE without leaving abort residue in MVCC.
    bool eraseDiscardingVersion(RowId rowId);
    bool replaceRow(RowId rowId, Row row);
    bool revive(RowId rowId, Row row);
    // Apply a physical redo after-image or erase during WAL recovery.
    bool applyPhysicalUpsert(RowId rowId, Row row);
    bool applyPhysicalErase(RowId rowId);
    // Apply page-image redo (heap pages + index pages) during WAL recovery.
    void applyPageImageRedo(bool hasHeapMeta, std::size_t capacity, std::vector<RowId> freeList,
                            std::vector<std::pair<PageId, std::vector<std::byte>>> heapPages,
                            std::vector<std::pair<std::string, BTreeIndexSnapshot>> btreeIndexes,
                            std::vector<std::pair<std::string, HashIndexSnapshot>> hashIndexes);
    [[nodiscard]] std::optional<Row> getRow(RowId rowId) const;
    bool createIndex(std::string name, std::string column);
    bool createIndex(std::string name, IndexExpression expression);
    // Register index metadata without rebuilding (snapshot v4 restore path).
    bool createIndexWithoutRebuild(std::string name, std::string column);
    bool createIndexWithoutRebuild(std::string name, IndexExpression expression);
    void replaceRows(std::vector<Row> rows);
    void replaceSparse(std::size_t capacity, std::vector<RowId> freeList,
                       std::vector<std::pair<RowId, Row>> entries);
    [[nodiscard]] PageStoreSnapshot exportPageStore() const;
    void replaceFromPages(PageStoreSnapshot snapshot);
    void replaceFromPages(PageStoreSnapshot snapshot, bool rebuildIndexesAfter);
    [[nodiscard]] TableIndexStoreSnapshot exportIndexPages() const;
    void replaceIndexPages(TableIndexStoreSnapshot snapshot);
    void clearDirtyTracking();
    // Capture dirty heap/index pages after DML for PageImageRedo WAL payloads.
    [[nodiscard]] PageImageCapture takePageImageCapture();

  private:
    void addRowToIndexes(RowId rowId);
    void rebuildIndexes();
    void refreshVersionsFromStore();
    [[nodiscard]] Value indexKeyForRow(const std::string &indexName, const Row &row) const;
    bool registerIndex(std::string name, std::size_t columnIndex,
                       std::optional<IndexExpression> expression, bool rebuild);

    std::string name_;
    std::vector<Column> schema_;
    std::unique_ptr<RowStore> rowStore_;
    std::map<std::string, std::size_t> indexColumns_;
    std::map<std::string, IndexExpression> indexExpressions_;
    std::map<std::string, HashIndex> indexes_;
    std::map<std::string, BTreeIndex> orderedIndexes_;
    std::map<std::string, ColumnHistogram> histograms_;
    MVCCRowStore versions_;
    mutable std::shared_mutex mutex_;
};

} // namespace VertexDB
