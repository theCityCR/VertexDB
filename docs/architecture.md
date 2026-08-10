# Architecture

```text
CLI
 |
 SQL Parser
 |
 Query Executor
 |
 +-- Query Planner
 |   +-- Rewriter (CTE inline/materialize, IN subquery prep)
 |   +-- Access-path selection
 |
 +-- Storage Engine
 |   +-- Database
 |   +-- Table
 |   +-- RowStore
 |   |   +-- PageRowStore (default)
 |   |   +-- VectorRowStore
 |   +-- Row
 |   +-- BufferPool
 |
 +-- Index Manager
 |   +-- HashIndex
 |   +-- BTreeIndex
 |       +-- BTreeNode layout metadata
 |
 +-- Persistence Layer
 |   +-- StorageManager (path / open / rename)
 |   +-- TcrdbCodec (.tcrdb v1–v4 orchestrator)
 |       +-- Value, table, and index codecs
 |   +-- WriteAheadLog
 |
 +-- Concurrency
 |   +-- LockManager
 |
 +-- Transactions
     +-- TransactionManager
     +-- MVCCRowStore
     +-- UndoLog
```

## Module Responsibilities

- `common`: shared value types, column metadata, `IndexExpression` shapes/helpers, string helpers
  (`equalsIgnoreCase`), LIKE/regex/trigram pattern helpers (`string_pattern`), and binary POD I/O
  (`writePod` / `readPod` for streams and byte spans).
- `parser`: tokenization, AST construction, and SQL grammar validation (dispatch, DDL, DML, and
  predicate parsing live in focused translation units behind one `Parser` type). AST statements
  reference `IndexExpression` from `common/` rather than owning the type.

- `planner`: CTE/derived-table rewrite (inline or `AS MATERIALIZED`), correlated/`IN` prep, and
  cost-based access-path / join selection through the `RelationStats` and `IndexCatalogView`
  interfaces, including optional `ANALYZE` histograms, multi-index AND intersect / OR union
  (including partial OR with residual complementary scan), prefix `LIKE` / trigram intersect, and
  residual filters. `QueryPlanner` is split across `query_planner.cpp` (SELECT paths),
  `planner_predicate.cpp`, `query_planner_join.cpp`, and `query_planner_format.cpp`.

SQL predicates are a recursive `std::variant`: each comparison, boolean connective, list/subquery,
existence, `LIKE`, or regex node owns only the fields valid for that shape. Physical access paths
are likewise a variant (`FullScanPlan`, equality/range/IN/prefix-LIKE probes, intersection, or
union), while estimates and residual filters live in the shared `PlanEstimates` metadata.
- `storage`: database/table ownership, row storage boundaries, schema validation, `TableStatistics`,
  snapshot/redo logic in `TableSnapshotIO`, and page cache abstractions. `Table` is the synchronized
  façade over row, index, statistics, snapshot I/O, and MVCC components. `VectorRowStore` and
  `PageRowStore` are separate TUs sharing sparse-layout validation.
- `execution`: `QueryExecutor` is a stable façade that composes focused execution types.
  `SelectEngine` owns SELECT/join/EXPLAIN execution, `SubqueryRuntime` owns CTE/`IN`/`EXISTS`
  preparation and evaluation, and `PreparedStatementCatalog` owns parsed prepared ASTs.
  `TxnSession` owns transaction-manager, snapshot, undo-log, and deferred-WAL state, while
  `RecoveryService` owns WAL replay, redo/undo application, and WAL flushing. `predicate_eval`,
  `select_helpers` / `select_scope` / `select_aggregate`, `prepared_bind`, and `sql_literal`
  provide shared execution helpers.
- `indexing`: `IndexManager` owns index definitions plus hash/B+ tree stores and performs index
  maintenance against a caller-provided schema and `RowStore`. `Table` retains mutex ownership and
  forwards its public index API while holding the appropriate lock. `BTreeIndex` is split across
  `btree_index_{lookup,mutate,snapshot}.cpp`.
- `persistence`: `StorageManager` orchestrates snapshot paths; the slim `tcrdb_codec` orchestrates
  `.tcrdb` v1–v4 encode/decode across focused value, table, and index codec translation units.
  WAL recovery uses page-image redo plus legacy physical/logical records.
- `concurrency`: executor-level reader/writer synchronization via `LockManager`.
- `transaction`: commit sequences, MVCC row versions, and per-transaction undo-log rollback.

Public module headers carry a short ownership banner pointing at sibling TUs when useful;
see `AGENTS.md` for the agent-oriented layout map.

## Architectural Boundaries

`Table` owns schema validation and synchronization, composes `IndexManager`, `TableStatistics`, and
`TableSnapshotIO`, and delegates physical row storage to the `RowStore` interface. `TableSnapshotIO`
owns the unlocked snapshot restore/export, dirty-page capture, and physical/page-image redo logic;
`Table` keeps the public API and locks before forwarding Table-owned state. It implements the
read-only `RelationStats` and `IndexCatalogView` planner boundaries, so planning does not depend on
table storage internals. `PageRowStore` is the default implementation: serialized page payloads in
an in-memory page directory are the source of truth, and the LRU `BufferPool` is the access cache
(fill-on-miss). Reads deserialize live row slots from those page bytes. Each page holds a fixed
number of row slots; serialized page byte lengths vary with row content. Both row-store
implementations assign stable row IDs: deletes leave tombstones and push IDs onto a free list, and
inserts reuse freed IDs before growing capacity. Snapshots (format v4) persist `rowsPerPage`,
capacity, free-list order, serialized page-directory payloads, and index pages (B+ tree nodes and
hash buckets) so IDs, page bytes, and indexes survive save/load. Per-column equi-height histograms
from `ANALYZE` are persisted after index pages in v4 (optional `VDBHIST1` section). `VectorRowStore`
remains available as a simple in-memory implementation for focused tests or future comparisons.

`BTreeIndex` keeps the existing ordered lookup API while maintaining `BTreeNode` layout metadata
with page ids, leaf links, internal children, separator keys, and row-id payloads in leaves. Inserts
and deletes split and merge nodes incrementally; point lookups descend from the root and range scans
follow linked leaves. `exportPages` / `replaceFromPages` round-trip that layout for snapshots and
page-image redo.

`Table` exposes transaction-aware snapshots through `rowsSnapshot(ReadSnapshot, TransactionManager)`
and `rowsById(..., ReadSnapshot, TransactionManager)`. `TxnSession` stamps DML with SQL transaction
ids, captures a commit-seq snapshot at `BEGIN`, and supplies all SELECT visibility state for
commit-aware MVCC (including dirty-read prevention for concurrent autocommit readers).
`BEGIN`/`COMMIT`/`ROLLBACK` still use a per-transaction undo log for abort: DML records compensating
actions, `RecoveryService` applies them LIFO on `ROLLBACK` to the same `Database` instance, and
`COMMIT` discards the log after `RecoveryService` flushes deferred WAL.

On v4 `LOAD`, indexes are registered without rebuilding, heap page payloads are restored, then index
pages are installed from the snapshot. On v1–v3 `LOAD`, indexes are registered before rows so
`replaceFromPages` / `replaceSparse` / `replaceRows` rebuilds index entries from the restored row set.

## Current Limitations

- WAL recovery applies page-image redo for DML (DDL remains logical SQL); legacy `PhysicalRedo` and
  logical DML remain replayable. Trailing torn WAL records are skipped.
- Transactions provide commit-aware MVCC snapshot isolation for reads plus undo-log DML rollback;
  DML WAL records are deferred until `COMMIT` (one atomic batch) and dropped on `ROLLBACK`.

## Current Data Flow

1. The CLI reads a SQL string.
2. `Tokenizer` emits a token stream.
3. `Parser` creates a strongly typed `Query` variant (including aggregates/`GROUP BY`, multi-join
   chains with `INNER`/`LEFT` and non-equi `ON`, `WITH` materialize modes and nesting depth up to 3,
   `IN`/`EXISTS`, `LIKE`/`~`, expression indexes including trigram, and `EXPLAIN`). Prepared
   statements store that AST with `?` parameter slots for later binding.
4. For `SELECT`/`EXPLAIN`, a rewriter inlines or materializes CTEs/derived tables (including nested
   `WITH` up to depth 3 and `WITH` inside `IN`/`EXISTS`); the executor materializes uncorrelated
   `IN` subqueries and evaluates correlated `IN`/`EXISTS` per outer row with up to four outer
   binding frames (including `FROM` / `JOIN` table aliases).
5. `QueryPlanner` chooses an access path (column or expression index, prefix `LIKE`, trigram
   intersect, residual filters) and per-join algorithms for left-deep `INNER`/`LEFT` chains;
   `SelectEngine` runs filters/joins, then optional hash aggregation, then `ORDER BY`/`LIMIT`.
6. Results are returned as `QueryResult` with columns, rows, and a status message.
