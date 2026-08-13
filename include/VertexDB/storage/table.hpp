#pragma once

// Table façade: schema/DML, MVCC, persistence, and planner-facing views.
// IndexManager, TableStatistics, and TableSnapshotIO own focused logic; Table owns synchronization.
// Implementation TUs: table.cpp (identity/DML/MVCC reads), table_constraints.cpp,
// table_schema.cpp (ALTER), table_recovery.cpp (undo/physical/page-image redo),
// table_indexes.cpp, table_persist.cpp (ANALYZE / snapshot I/O wrappers).

#include "VertexDB/common/comparison_operator.hpp"
#include "VertexDB/common/index_expression.hpp"
#include "VertexDB/common/value.hpp"
#include "VertexDB/indexing/index_manager.hpp"
#include "VertexDB/parser/predicate.hpp"
#include "VertexDB/storage/foreign_key.hpp"
#include "VertexDB/storage/histogram.hpp"
#include "VertexDB/storage/relation_stats.hpp"
#include "VertexDB/storage/row.hpp"
#include "VertexDB/storage/row_store.hpp"
#include "VertexDB/storage/table_snapshot_io.hpp"
#include "VertexDB/storage/table_statistics.hpp"
#include "VertexDB/storage/unique_constraint.hpp"
#include "VertexDB/transaction/mvcc_row_store.hpp"

#include <memory>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace VertexDB {

class Database;

class Table : public RelationStats, public IndexCatalogView {
  public:
    Table(std::string name, std::vector<Column> schema,
          std::vector<Predicate> checkConstraints = {},
          std::vector<ForeignKeyConstraint> foreignKeys = {},
          std::vector<UniqueConstraint> uniqueConstraints = {});

    // --- SQL-facing identity / schema metadata ---
    [[nodiscard]] const std::string &name() const noexcept;
    void setName(std::string name);
    [[nodiscard]] std::span<const Column> schema() const noexcept;
    [[nodiscard]] std::span<const Predicate> checkConstraints() const noexcept;
    [[nodiscard]] std::span<const ForeignKeyConstraint> foreignKeys() const noexcept;
    [[nodiscard]] std::span<const UniqueConstraint> uniqueConstraints() const noexcept;
    // Column-level UNIQUE/PK flags merged with table-level constraints (ordered column lists).
    [[nodiscard]] std::vector<UniqueConstraint> allUniqueConstraints() const;
    [[nodiscard]] std::optional<std::size_t> columnIndex(std::string_view column) const;
    void validateRow(const Row &row) const;
    // Reject duplicate values on UNIQUE / PRIMARY KEY (NULLs skipped for UNIQUE).
    void assertUniqueRow(const Row &row, std::optional<RowId> excludeRowId = std::nullopt) const;
    // True when two rows conflict on any UNIQUE / PRIMARY KEY constraint.
    [[nodiscard]] bool rowsConflictOnUnique(const Row &left, const Row &right) const;
    // Register reserved `__pk_` / `__uq_` indexes when missing (CREATE TABLE / restore).
    void ensureConstraintIndexes();

    // --- Schema evolution (ALTER TABLE; CatalogEngine) ---
    // Append a column and pad every live heap row + MVCC version with `fill`
    // (or NULL when fill is nullopt). NOT NULL without a fill requires an empty table.
    void addColumn(Column column, std::optional<Value> fill = std::nullopt);
    // Drop a column; with cascade, drops same-table dependents first. Returns capture for undo.
    struct DroppedColumnCapture {
        Column column;
        std::size_t columnIndex{};
        std::vector<std::pair<RowId, Value>> heapValues;
        std::vector<std::pair<RowId, std::vector<Value>>> versionValues;
        bool cascaded{false};
        std::vector<IndexDefinition> droppedUserIndexes;
        std::vector<Predicate> droppedChecks;
        std::vector<UniqueConstraint> droppedUniques;
        std::vector<ForeignKeyConstraint> droppedChildForeignKeys;
    };
    [[nodiscard]] DroppedColumnCapture dropColumn(std::string_view columnName,
                                                  const Database *database, bool cascade);
    // Restore a previously dropped column (ROLLBACK of DROP COLUMN), including cascaded deps.
    void restoreDroppedColumn(const DroppedColumnCapture &capture);
    // Rename a column across schema, indexes, CHECKs, UNIQUE/PK, child FKs, histograms;
    // also rewrites parent FK column names on other tables when `database` is set.
    void renameColumn(std::string_view oldName, std::string_view newName, Database *database);
    // Rewrite parentColumns when this table's FK references `parentTable.oldName`.
    void rewriteForeignKeyParentColumn(std::string_view parentTable, std::string_view oldName,
                                       std::string_view newName);

    // --- MVCC / visibility reads (SelectEngine / SSI) ---
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

    // --- SQL-facing DML ---
    // Optional `transactions` records SSI write sets and insert images for phantom checks.
    RowId insert(Row row, TransactionId writerId = kSystemTransactionId,
                 TransactionManager *transactions = nullptr);
    bool erase(RowId rowId, TransactionId writerId = kSystemTransactionId,
               TransactionManager *transactions = nullptr);
    bool update(RowId rowId, std::size_t columnIndex, Value value,
                TransactionId writerId = kSystemTransactionId,
                TransactionManager *transactions = nullptr);

    // --- Recovery / undo / page-image (preferred callers: RecoveryService, codecs,
    // TableSnapshotIO) ---
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

    // --- Planner views (RelationStats / IndexCatalogView) + index CRUD ---
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
    indexedLookup(std::span<const std::string> columns, const Value &key) const;
    [[nodiscard]] std::optional<std::vector<RowId>>
    indexedLookup(const IndexExpression &expression, const Value &value) const;
    [[nodiscard]] std::optional<std::vector<RowId>>
    orderedLookup(std::string_view column, ComparisonOperator op, const Value &value) const;
    [[nodiscard]] std::optional<std::vector<RowId>>
    orderedLookup(const IndexExpression &expression, ComparisonOperator op,
                  const Value &value) const;
    // True when the column has a maintained single-column index (hash equality + ordered range).
    [[nodiscard]] bool hasIndex(std::string_view column) const override;
    [[nodiscard]] bool hasIndex(std::span<const std::string> columns) const override;
    [[nodiscard]] std::optional<std::size_t>
    indexDistinctCount(std::span<const std::string> columns) const override;
    [[nodiscard]] std::vector<std::vector<std::string>>
    compositeIndexColumnLists() const override;
    [[nodiscard]] bool hasExpressionIndex(const IndexExpression &expression) const override;
    [[nodiscard]] std::vector<std::string> listIndexes() const;
    [[nodiscard]] std::vector<IndexDefinition> indexDefinitions() const;
    bool createIndex(std::string name, std::string column);
    bool createIndex(std::string name, std::vector<std::string> columns);
    bool createIndex(std::string name, IndexExpression expression);
    // Register index metadata without rebuilding (snapshot v4+ restore path).
    bool createIndexWithoutRebuild(std::string name, std::string column);
    bool createIndexWithoutRebuild(std::string name, std::vector<std::string> columns);
    bool createIndexWithoutRebuild(std::string name, IndexExpression expression);
    // Drop a named index (public DROP INDEX SQL and txn undo of CREATE INDEX).
    bool dropIndex(std::string_view name);
    // Observability for tests: ordered index node layout for a named index.
    [[nodiscard]] std::optional<std::vector<BTreeNode>>
    orderedIndexNodesSnapshot(std::string_view indexName) const;

    // --- Snapshot / page-image I/O (preferred callers: CatalogEngine SAVE/LOAD, codecs) ---
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
    bool registerIndex(std::string name, std::vector<std::size_t> columnIndexes,
                       std::optional<IndexExpression> expression, bool rebuild);
    bool registerIndex(std::string name, std::size_t columnIndex,
                       std::optional<IndexExpression> expression, bool rebuild);
    void enforceUniqueConstraintsUnlocked(const Row &row,
                                          std::optional<RowId> excludeRowId) const;
    void enforceCheckConstraints(const Row &row) const;
    [[nodiscard]] static std::string constraintIndexName(const UniqueConstraint &constraint);
    [[nodiscard]] static std::string formatUniqueColumns(const UniqueConstraint &constraint);
    [[nodiscard]] Value uniqueKeyForRow(const UniqueConstraint &constraint, const Row &row) const;
    [[nodiscard]] bool uniqueRowsEqual(const UniqueConstraint &constraint, const Row &left,
                                       const Row &right) const;

    std::string name_;
    std::vector<Column> schema_;
    std::vector<Predicate> checkConstraints_;
    std::vector<ForeignKeyConstraint> foreignKeys_;
    std::vector<UniqueConstraint> uniqueConstraints_;
    std::unique_ptr<RowStore> rowStore_;
    IndexManager indexManager_;
    TableStatistics statistics_;
    TableSnapshotIO snapshotIo_;
    MVCCRowStore versions_;
    mutable std::shared_mutex mutex_;
};

} // namespace VertexDB
