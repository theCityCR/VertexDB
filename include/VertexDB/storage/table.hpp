#pragma once

// Table façade: schema/DML, MVCC, persistence, and planner-facing views.
// IndexManager, TableStatistics, and TableSnapshotIO own focused logic; Table owns synchronization.

#include "VertexDB/common/comparison_operator.hpp"
#include "VertexDB/common/index_expression.hpp"
#include "VertexDB/common/value.hpp"
#include "VertexDB/indexing/index_manager.hpp"
#include "VertexDB/storage/histogram.hpp"
#include "VertexDB/storage/relation_stats.hpp"
#include "VertexDB/storage/row.hpp"
#include "VertexDB/storage/row_store.hpp"
#include "VertexDB/storage/table_snapshot_io.hpp"
#include "VertexDB/storage/table_statistics.hpp"
#include "VertexDB/transaction/mvcc_row_store.hpp"

#include <memory>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace VertexDB {

class Table : public RelationStats, public IndexCatalogView {
  public:
    Table(std::string name, std::vector<Column> schema);

    // --- Identity / schema ---
    [[nodiscard]] const std::string &name() const noexcept;
    void setName(std::string name);
    [[nodiscard]] std::span<const Column> schema() const noexcept;
    [[nodiscard]] std::optional<std::size_t> columnIndex(std::string_view column) const;
    void validateRow(const Row &row) const;
    // Reject duplicate values on UNIQUE / PRIMARY KEY columns (NULLs skipped for UNIQUE).
    void assertUniqueRow(const Row &row, std::optional<RowId> excludeRowId = std::nullopt) const;
    // Register reserved `__pk_` / `__uq_` column indexes when missing (CREATE TABLE / restore).
    void ensureConstraintIndexes();

    // --- MVCC / visibility reads ---
    [[nodiscard]] std::vector<Row> rowsSnapshot() const;
    // Snapshot reads record row-level SSI read sets for active `snapshot.self` transactions.
    [[nodiscard]] std::vector<Row> rowsSnapshot(const ReadSnapshot &snapshot,
                                                TransactionManager &transactions) const;
    [[nodiscard]] std::vector<std::pair<RowId, Row>> liveEntries() const;
    [[nodiscard]] std::vector<std::pair<RowId, Row>>
    visibleEntries(const ReadSnapshot &snapshot, TransactionManager &transactions) const;
    [[nodiscard]] std::vector<RowId> freeList() const;
    [[nodiscard]] std::vector<Row> rowsById(std::span<const RowId> rowIds) const;
    [[nodiscard]] std::vector<Row> rowsById(std::span<const RowId> rowIds,
                                            const ReadSnapshot &snapshot,
                                            TransactionManager &transactions) const;
    [[nodiscard]] std::vector<std::pair<RowId, Row>>
    visibleEntriesById(std::span<const RowId> rowIds, const ReadSnapshot &snapshot,
                       TransactionManager &transactions) const;
    [[nodiscard]] std::size_t rowCount() const override;
    [[nodiscard]] std::size_t capacity() const;
    [[nodiscard]] std::optional<Row> getRow(RowId rowId) const;
    [[nodiscard]] std::size_t versionCount(RowId rowId) const;

    // --- DML ---
    // Optional `transactions` records SSI write sets and insert images for phantom checks.
    RowId insert(Row row, TransactionId writerId = kSystemTransactionId,
                 TransactionManager *transactions = nullptr);
    bool erase(RowId rowId, TransactionId writerId = kSystemTransactionId,
               TransactionManager *transactions = nullptr);
    bool update(RowId rowId, std::size_t columnIndex, Value value,
                TransactionId writerId = kSystemTransactionId,
                TransactionManager *transactions = nullptr);

    // --- Undo / recovery helpers ---
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

    // --- Indexes / ANALYZE / planner views ---
    // On-demand stats for cost-based planning (row count + per-index distinct keys).
    [[nodiscard]] std::optional<std::size_t>
    indexDistinctCount(std::string_view column) const override;
    [[nodiscard]] std::optional<std::size_t>
    indexDistinctCount(const IndexExpression &expression) const override;
    // ANALYZE builds equi-height per-column histograms (+ distinct counts) from live rows.
    void analyze(std::size_t maxBuckets = kDefaultHistogramBuckets);
    [[nodiscard]] std::optional<ColumnHistogram>
    columnHistogram(std::string_view column) const override;
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
    [[nodiscard]] bool hasIndex(std::string_view column) const override;
    [[nodiscard]] bool hasExpressionIndex(const IndexExpression &expression) const override;
    [[nodiscard]] std::vector<std::string> listIndexes() const;
    [[nodiscard]] std::vector<IndexDefinition> indexDefinitions() const;
    bool createIndex(std::string name, std::string column);
    bool createIndex(std::string name, IndexExpression expression);
    // Register index metadata without rebuilding (snapshot v4/v5 restore path).
    bool createIndexWithoutRebuild(std::string name, std::string column);
    bool createIndexWithoutRebuild(std::string name, IndexExpression expression);
    // Drop a named index (public DROP INDEX SQL and txn undo of CREATE INDEX).
    bool dropIndex(std::string_view name);
    // Observability for tests: ordered index node layout for a named index.
    [[nodiscard]] std::optional<std::vector<BTreeNode>>
    orderedIndexNodesSnapshot(std::string_view indexName) const;

    // --- Snapshot / page-image I/O ---
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
    bool registerIndex(std::string name, std::size_t columnIndex,
                       std::optional<IndexExpression> expression, bool rebuild);
    void enforceUniqueConstraintsUnlocked(const Row &row,
                                          std::optional<RowId> excludeRowId) const;

    std::string name_;
    std::vector<Column> schema_;
    std::unique_ptr<RowStore> rowStore_;
    IndexManager indexManager_;
    TableStatistics statistics_;
    TableSnapshotIO snapshotIo_;
    MVCCRowStore versions_;
    mutable std::shared_mutex mutex_;
};

} // namespace VertexDB
