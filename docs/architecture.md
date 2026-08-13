# Architecture

```text
CLI
 |
 SQL Parser
 |
 Query Executor
 |   +-- ExecutionContext (DB / planner / session + Select/Subquery peers)
 |   +-- SelectEngine / SubqueryRuntime / DmlEngine / CatalogEngine
 |
 +-- Query Planner
 |   +-- Rewriter (CTE inline/materialize, IN subquery prep)
 |   +-- Access-path selection
 |
 +-- Storage Engine
 |   +-- Database
 |   +-- Table
 |   +-- RowStore
 |   |   +-- PageRowStore (default; CRUD / buffer / io TUs)
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
- `parser`: tokenization, AST construction, and SQL grammar validation (dispatch, DDL, DML,
  SELECT, WITH/CTE, outer-ref marking, and predicate parsing live in focused translation units
  behind one `Parser` type). AST statements reference `IndexExpression` from `common/` rather
  than owning the type.

- `planner`: CTE/derived-table rewrite (inline, `AS MATERIALIZED`, force-materialize for outer
  `JOIN` targets and `WITH RECURSIVE`), correlated/`IN` prep, set operations, and cost-based access-path /
  join selection through the `RelationStats` and `IndexCatalogView` interfaces, including optional
  `ANALYZE` histograms, multi-index AND intersect / OR union (including partial OR with residual
  complementary scan and composite Intersect∪Union for nested OR under AND, including partial
  nested OR),
  prefix `LIKE` / trigram intersect, and residual filters. `EXPLAIN` /
  `EXPLAIN ANALYZE` formatting lives in `query_planner_format.cpp`. `QueryPlanner` is
  split across `query_planner.cpp` (thin wrappers), `query_planner_select.cpp` (`planSelect`
  orchestration), `query_planner_access.cpp` (OR-union / AND-intersect / best-path finalize),
  `query_planner_predicate.cpp`, `query_planner_join.cpp`, and `query_planner_format.cpp`.

SQL predicates are a recursive `std::variant`: each comparison, boolean connective, list/subquery,
existence, `LIKE`, or regex node owns only the fields valid for that shape. Physical access paths
are likewise a variant (`FullScanPlan`, equality/range/IN/prefix-LIKE probes, intersection, or
union), while estimates and residual filters live in the shared `PlanEstimates` metadata.
- `storage`: database/table ownership, row storage boundaries, schema validation, `TableStatistics`,
  snapshot/redo logic in `TableSnapshotIO`, and page cache abstractions. `Table` is the synchronized
  façade over row, index, statistics, snapshot I/O, and MVCC components. `RowStore` (interface) lives
  in `row_store.hpp`; `VectorRowStore` and `PageRowStore` have dedicated headers. `PageRowStore` is
  split across `page_row_store.cpp` (CRUD), `page_row_store_buffer.cpp` (pool/dirty), and
  `page_row_store_io.cpp` (encode/decode/replace/redo), sharing sparse-layout validation with
  `VectorRowStore`.
- `execution`: `QueryExecutor` is a stable façade that composes focused execution types.
  `ExecutionContext` holds non-owning refs to the database, planner, and txn session plus peer
  pointers to `SelectEngine` / `SubqueryRuntime` (no `QueryExecutor` friendship).
  `SelectEngine` owns SELECT/join/`EXPLAIN`/`EXPLAIN ANALYZE` execution (`select_engine.cpp`
  orchestration, `select_engine_scan.cpp`, `select_engine_join.cpp`). `DmlEngine` owns
  INSERT/UPDATE/DELETE with undo and page-image WAL redo; UPDATE/DELETE reuse
  `QueryPlanner::planSelect` plus `SelectEngine::collectVisibleEntries` so mutation `WHERE`
  clauses use the same index access paths as SELECT. `CatalogEngine` owns CREATE/DROP/RENAME
  DATABASE/TABLE, `LIST TABLES`, `CREATE INDEX` / `DROP INDEX`, `ANALYZE`, and `SAVE`/`LOAD` (with
  WAL append and snapshot coordination). `SubqueryRuntime` owns CTE/`IN`/`EXISTS` preparation and
  evaluation (`subquery_runtime.cpp`, `subquery_runtime_bind.cpp`, `subquery_runtime_cte.cpp`),
  including joined subqueries, recursive CTE materialization, and full predicate matching
  (correlated subquery arms). `PreparedStatementCatalog` owns parsed prepared ASTs.
  `TxnSession` owns transaction-manager, snapshot, undo-log, and deferred-WAL state, while
  `RecoveryService` owns WAL replay, redo/undo application, and WAL flushing. `predicate_eval`,
  `select_helpers` / `select_scope` / `select_aggregate`, `prepared_bind`, and `sql_literal`
  provide shared execution helpers.
- `indexing`: `IndexManager` owns index definitions plus hash/B+ tree stores and performs index
  maintenance against a caller-provided schema and `RowStore`. `Table` retains mutex ownership and
  forwards its public index API while holding the appropriate lock. `BTreeIndex` is split across
  `btree_index_{lookup,mutate,snapshot}.cpp`.
- `persistence`: `StorageManager` orchestrates snapshot paths with durable publish (shared
  `durable_sync` file/directory helpers also used by `WriteAheadLog`); the slim `tcrdb_codec`
  orchestrates `.tcrdb` v1–v8 encode/decode across focused value, table, and index codec translation
  units. WAL recovery uses page-image redo plus legacy physical/logical records.
  `WriteAheadLog::append` and `reset` flush+fsync for durable COMMIT / autocommit; `SAVE` fsyncs the
  temp snapshot before rename and syncs the storage directory on POSIX.
- `concurrency`: executor-level reader/writer synchronization via `LockManager` (shared readers;
  exclusive writers). One `QueryExecutor` holds at most one open SQL transaction.
- `transaction`: commit sequences, MVCC row versions, per-transaction undo-log rollback, and
  SSI checks at commit (row read/write sets plus insert-phantom predicate SIREAD vs insert/update
  images). Snapshot isolation prevents dirty reads and hides post-`BEGIN` commits; SSI aborts
  write skew / write–write overlap and matching insert phantoms. Multi-txn interleaving tests share
  a `Table` + `TransactionManager` directly — see [si_anomaly_wedge.md](si_anomaly_wedge.md).

Public module headers carry a short ownership banner pointing at sibling TUs when useful;
see `AGENTS.md` for the agent-oriented layout map.

## Architectural Boundaries

`Table` owns schema validation (including `NOT NULL`, single- and multi-column `PRIMARY KEY` / `UNIQUE`, and
simple `CHECK` enforcement) and stores `FOREIGN KEY` metadata;
`CatalogEngine` / `DmlEngine` enforce referential integrity with SI-visible parent lookups.
`Table` owns
synchronization, composes `IndexManager`, `TableStatistics`, and
`TableSnapshotIO`, and delegates physical row storage to the `RowStore` interface. `TableSnapshotIO`
owns the unlocked snapshot restore/export, dirty-page capture, and physical/page-image redo logic;
`Table` keeps the public API and locks before forwarding Table-owned state. It implements the
read-only `RelationStats` and `IndexCatalogView` planner boundaries, so planning does not depend on
table storage internals. `PageRowStore` is the default implementation: serialized page payloads in
an in-memory page directory are the source of truth, and the LRU `BufferPool` is the access cache
(fill-on-miss). Reads deserialize live row slots from those page bytes. Each page holds a fixed
number of row slots; serialized page byte lengths vary with row content. Both row-store
implementations assign stable row IDs: deletes leave tombstones and push IDs onto a free list, and
inserts reuse freed IDs before growing capacity. Snapshots (format v8; v7–v1 still loadable) persist
`rowsPerPage`, capacity, free-list order, serialized page-directory payloads, index pages (B+ tree
nodes and hash buckets), per-column uniqueness / primary-key flags, table-level composite unique
constraints, `CHECK` predicate text, and
`FOREIGN KEY` metadata so
IDs, page bytes, indexes,
and constraints survive save/load. Per-column equi-height histograms from `ANALYZE` are persisted
after index pages in v4+ (optional `VDBHIST1` section). `VectorRowStore`
remains available as a simple in-memory implementation for focused tests or future comparisons.

`BTreeIndex` keeps the existing ordered lookup API while maintaining `BTreeNode` layout metadata
with page ids, leaf links, internal children, separator keys, and row-id payloads in leaves. Inserts
and deletes split and merge nodes incrementally; point lookups descend from the root and range scans
follow linked leaves. `exportPages` / `replaceFromPages` round-trip that layout for snapshots and
page-image redo.

`Table` exposes transaction-aware snapshots through `rowsSnapshot(ReadSnapshot, TransactionManager)`
and `rowsById(..., ReadSnapshot, TransactionManager)`. `TxnSession` stamps DML with SQL transaction
ids, captures a commit-seq snapshot at `BEGIN`, and supplies all SELECT visibility state for
commit-aware MVCC (dirty-read prevention, SI watermark for post-`BEGIN` commits, and held-snapshot
phantom hiding). Write skew remains allowed under classic SI.
`BEGIN`/`COMMIT`/`ROLLBACK` still use a per-transaction undo log for abort: DML and transactional
catalog DDL (`CREATE`/`DROP`/`RENAME TABLE`, `CREATE DATABASE`, `CREATE`/`DROP INDEX`) record
compensating actions, `RecoveryService` applies them LIFO on `ROLLBACK` to the same `Database`
instance (or restores a prior DB after `CREATE DATABASE`), and `COMMIT` discards the log after
`RecoveryService` flushes deferred WAL (logical DDL SQL plus collapsed page-image redo) with
durable sync before discarding the undo log. Crash-injection on `COMMIT` can kill after that WAL
sync but before the in-memory commit mark (or before WAL sync) for durability tests.
`SAVE DATABASE` may implicitly commit first; `LOAD DATABASE` may implicitly roll back first.

On v4 `LOAD`, indexes are registered without rebuilding, heap page payloads are restored, then index
pages are installed from the snapshot. On v1–v3 `LOAD`, indexes are registered before rows so
`replaceFromPages` / `replaceSparse` / `replaceRows` rebuilds index entries from the restored row set.

## Current Limitations

- WAL recovery applies page-image redo for DML (DDL remains logical SQL); legacy `PhysicalRedo` and
  logical DML remain replayable. Trailing torn WAL records are skipped. Successful WAL
  `append`/`reset` and snapshot `SAVE` share `durable_sync` (file flush+fsync / `F_FULLFSYNC` on
  macOS; Windows `FlushFileBuffers` on the file). WAL also syncs its parent directory when the file
  is newly created on POSIX; `SAVE` syncs the storage directory after rename on POSIX.
- Transactions provide commit-aware MVCC snapshot isolation for reads plus undo-log rollback for
  DML and transactional catalog DDL (`CREATE`/`DROP`/`RENAME TABLE`, `CREATE DATABASE`,
  `CREATE`/`DROP INDEX`); DML WAL records are deferred until `COMMIT` (one atomic batch) and dropped
  on `ROLLBACK`. `SAVE DATABASE` in an open transaction implicitly commits then checkpoints;
  `LOAD DATABASE` implicitly rolls back then loads. SSI aborts write skew / write–write overlap and
  insert phantoms (predicate SIREAD, including OR of column leaves and column `LIKE`; regex /
  subquery / expression-index probes still use relation membership); there are no row/page locks or
  Postgres next-key locks.

## Current Data Flow

1. The CLI reads a SQL string.
2. `Tokenizer` emits a token stream.
3. `Parser` creates a strongly typed `Query` variant (including aggregates/`GROUP BY`, multi-join
   chains with `INNER`/`LEFT`/`RIGHT`/`FULL`/`CROSS` and non-equi `ON`, `WITH` materialize modes,
   nesting depth up to 6, and `WITH RECURSIVE` (`UNION` / `UNION ALL`, independent or mutual
   recursive CTEs, optional `AS ACCUMULATOR`), `IN`/`EXISTS`, set ops (`UNION`/`INTERSECT`/`EXCEPT` including `ALL`), `LIKE`/`~`, expression
   indexes including trigram, `DROP DATABASE`, and `EXPLAIN` / `EXPLAIN ANALYZE` / `EXPLAIN INSERT`). Prepared statements store that
   AST with `?` parameter slots for later binding.
4. For `SELECT`/`EXPLAIN`/`EXPLAIN ANALYZE`, a rewriter inlines or materializes CTEs/derived tables
   (including nested `WITH` up to depth 6, `WITH`/`JOIN` inside `IN`/`EXISTS`, outer `JOIN` against
   CTE/derived aliases, and `WITH RECURSIVE`); the executor materializes uncorrelated `IN`
   subqueries and evaluates correlated `IN`/`EXISTS` per outer row with up to eight outer binding
   frames (including `FROM` / `JOIN` table aliases), routing joined subqueries through
   `executeJoinSelect`.
5. `QueryPlanner` chooses an access path (column or expression index, multi-index AND intersect /
   OR union / composite Intersect∪Union (incl. partial nested OR under AND), prefix `LIKE`,
   trigram intersect, residual filters) and
   per-join algorithms for
   left-deep `INNER`/`LEFT`/`RIGHT`/`FULL`/`CROSS` chains; `SelectEngine` runs filters/joins, then
   optional hash aggregation, then `ORDER BY`/`LIMIT`. `EXPLAIN ANALYZE` executes once and appends
   `actual_rows` / optional residual `candidates` / `actual_time_ms` beside `est_rows`/`cost`.
6. Results are returned as `QueryResult` with columns, rows, and a status message.
