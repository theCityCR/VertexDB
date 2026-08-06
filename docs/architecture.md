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
 |   +-- StorageManager
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

- `common`: shared value types, column metadata, string helpers (`equalsIgnoreCase`), and binary
  POD I/O (`writePod` / `readPod` for streams and byte spans).
- `parser`: tokenization, AST construction, and SQL grammar validation (dispatch, DDL, DML, and
  predicate parsing live in focused translation units behind one `Parser` type).
- `planner`: CTE/derived-table rewrite (inline or `AS MATERIALIZED`), correlated/`IN` prep, and
  cost-based access-path / join selection using table and index statistics (row counts and distinct
  keys), with residual filters.
- `storage`: database/table ownership, row storage boundaries, schema validation, and page cache
  abstractions (`VectorRowStore` and `PageRowStore` are separate TUs sharing sparse-layout
  validation).
- `execution`: `QueryExecutor` façade for command dispatch; helpers for predicate evaluation,
  SELECT projection/ORDER BY/LIMIT, SQL/WAL literal formatting, and startup recovery.
- `indexing`: hash indexes and ordered B+ tree index APIs with explicit node/page layout metadata.
- `persistence`: binary serialization (`.tcrdb` snapshots v1–v4), versioning, save/load, and WAL
  recovery (page-image redo plus legacy physical/logical records).
- `concurrency`: executor-level reader/writer synchronization via `LockManager`.
- `transaction`: commit sequences, MVCC row versions, and per-transaction undo-log rollback.

## Architectural Boundaries

`Table` owns schema validation and index maintenance, but delegates physical row storage to the
`RowStore` interface. `PageRowStore` is the default implementation: serialized page payloads in an
in-memory page directory are the source of truth, and the LRU `BufferPool` is the access cache
(fill-on-miss). Reads deserialize live row slots from those page bytes. Each page holds a fixed
number of row slots; serialized page byte lengths vary with row content. Both row-store
implementations assign stable row IDs: deletes leave tombstones and push IDs onto a free list, and
inserts reuse freed IDs before growing capacity. Snapshots (format v4) persist `rowsPerPage`,
capacity, free-list order, serialized page-directory payloads, and index pages (B+ tree nodes and
hash buckets) so IDs, page bytes, and indexes survive save/load. `VectorRowStore` remains available
as a simple in-memory implementation for focused tests or future comparisons.

`BTreeIndex` keeps the existing ordered lookup API while maintaining `BTreeNode` layout metadata
with page ids, leaf links, internal children, separator keys, and row-id payloads in leaves. Inserts
and deletes split and merge nodes incrementally; point lookups descend from the root and range scans
follow linked leaves. `exportPages` / `replaceFromPages` round-trip that layout for snapshots and
page-image redo.

`Table` exposes transaction-aware snapshots through `rowsSnapshot(ReadSnapshot, TransactionManager)`
and `rowsById(..., ReadSnapshot, TransactionManager)`. The executor stamps DML with SQL transaction
ids, captures a commit-seq snapshot at `BEGIN`, and routes all SELECT visibility through commit-aware
MVCC (including dirty-read prevention for concurrent autocommit readers). `BEGIN`/`COMMIT`/`ROLLBACK`
still use a per-transaction undo log for abort: DML records compensating actions, `ROLLBACK` applies
them LIFO on the same `Database` instance, and `COMMIT` discards the log.

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
3. `Parser` creates a strongly typed `Query` variant (including `WITH` materialize modes,
   `IN`/`EXISTS`, expression indexes, and `EXPLAIN`).
4. For `SELECT`/`EXPLAIN`, a rewriter inlines or materializes CTEs/derived tables; the executor
   materializes uncorrelated `IN` subqueries and evaluates single-level correlated `IN`/`EXISTS`
   per outer row.
5. `QueryPlanner` chooses an access path (column or expression index, residual filters) and
   `QueryExecutor` runs it.
6. Results are returned as `QueryResult` with columns, rows, and a status message.
