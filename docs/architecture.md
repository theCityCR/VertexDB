# Architecture

```text
CLI
 |
 SQL Parser
 |
 Query Executor
 |
 +-- Query Planner
 |   +-- Rewriter (CTE inlining, IN subquery prep)
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
- `planner`: CTE/`IN` rewrite and cost-based access-path / join selection using table and index
  statistics (row counts and distinct keys), with residual filters.
- `storage`: database/table ownership, row storage boundaries, schema validation, and page cache
  abstractions (`VectorRowStore` and `PageRowStore` are separate TUs sharing sparse-layout
  validation).
- `execution`: `QueryExecutor` façade for command dispatch; helpers for predicate evaluation,
  SELECT projection/ORDER BY/LIMIT, SQL/WAL literal formatting, and startup recovery.
- `indexing`: hash indexes and ordered B+ tree index APIs with explicit node/page layout metadata.
- `persistence`: binary serialization (`.tcrdb` snapshots), versioning, save/load, and logical WAL
  recovery.
- `concurrency`: executor-level reader/writer synchronization via `LockManager`.
- `transaction`: commit sequences, MVCC row versions, and per-transaction undo-log rollback.

## Architectural Boundaries

`Table` owns schema validation and index maintenance, but delegates physical row storage to the
`RowStore` interface. `PageRowStore` is the default implementation: serialized page payloads in an
in-memory page directory are the source of truth, and the LRU `BufferPool` is the access cache
(fill-on-miss). Reads deserialize live row slots from those page bytes. Each page holds a fixed
number of row slots; serialized page byte lengths vary with row content. Both row-store
implementations assign stable row IDs: deletes leave tombstones and push IDs onto a free list, and
inserts reuse freed IDs before growing capacity. Snapshots persist `rowsPerPage`, capacity, free-list
order, and serialized page-directory payloads so IDs and page bytes survive save/load. `VectorRowStore`
remains available as a simple in-memory implementation for focused tests or future comparisons.

`BTreeIndex` keeps the existing ordered lookup API while maintaining `BTreeNode` layout metadata
with page ids, leaf links, internal children, separator keys, and row-id payloads in leaves. Inserts
and deletes split and merge nodes incrementally; point lookups descend from the root and range scans
follow linked leaves.

`Table` exposes transaction-aware snapshots through `rowsSnapshot(ReadSnapshot, TransactionManager)`
and `rowsById(..., ReadSnapshot, TransactionManager)`. The executor stamps DML with SQL transaction
ids, captures a commit-seq snapshot at `BEGIN`, and routes all SELECT visibility through commit-aware
MVCC (including dirty-read prevention for concurrent autocommit readers). `BEGIN`/`COMMIT`/`ROLLBACK`
still use a per-transaction undo log for abort: DML records compensating actions, `ROLLBACK` applies
them LIFO on the same `Database` instance, and `COMMIT` discards the log.

On `LOAD`, indexes are registered on each table before page payloads (or legacy sparse/dense rows)
are reloaded so `replaceFromPages` / `replaceSparse` / `replaceRows` rebuilds index entries from the
restored row set.

## Current Limitations

- Index pages are not persisted; SAVE/LOAD rebuilds indexes from restored rows.
- WAL recovery applies physical row-image redo for DML (DDL remains logical SQL); trailing torn
  WAL records are skipped. Page-image redo is the next persistence step now that snapshots store
  page payloads.
- Transactions provide commit-aware MVCC snapshot isolation for reads plus undo-log DML rollback;
  DML WAL records are deferred until `COMMIT` (one atomic batch) and dropped on `ROLLBACK`.

## Current Data Flow

1. The CLI reads a SQL string.
2. `Tokenizer` emits a token stream.
3. `Parser` creates a strongly typed `Query` variant (including `WITH`, `IN` subqueries, and
   `EXPLAIN`).
4. For `SELECT`/`EXPLAIN`, a rewriter inlines CTEs and the executor materializes `IN` subqueries.
5. `QueryPlanner` chooses an access path (including residual filters) and `QueryExecutor` runs it.
6. Results are returned as `QueryResult` with columns, rows, and a status message.
