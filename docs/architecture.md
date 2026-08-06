# Architecture

```text
CLI
 |
 SQL Parser
 |
 Query Executor
 |
 +-- Storage Engine
 |   +-- Database
 |   +-- Table
 |   +-- RowStore
 |   |   +-- VectorRowStore
 |   |   +-- PageRowStore
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
 |
 +-- Concurrency
     +-- LockManager
     +-- TransactionManager
     +-- MVCCRowStore
```

## Module Responsibilities

- `common`: shared value types, column metadata, and low-level utilities.
- `parser`: tokenization, AST construction, and SQL grammar validation.
- `storage`: database/table ownership, row storage boundaries, schema validation, and page cache
  abstractions.
- `execution`: command dispatch, predicate evaluation, projection, and result construction.
- `indexing`: hash indexes and ordered B+ tree index APIs with explicit node/page layout metadata.
- `persistence`: binary serialization, versioning, save/load, and recovery.
- `concurrency`: reader/writer synchronization and transaction coordination.

## Architectural Boundaries

`Table` owns schema validation and index maintenance, but delegates physical row storage to the
`RowStore` interface. `PageRowStore` is the default implementation: serialized page payloads in an
in-memory page directory are the source of truth, and the LRU `BufferPool` is the access cache
(fill-on-miss). Reads deserialize live row slots from those page bytes. Both row-store
implementations assign stable row IDs: deletes leave tombstones and push IDs onto a free list, and
inserts reuse freed IDs before growing capacity. Snapshots persist capacity, free-list order, and
live `(rowId, row)` entries so IDs survive save/load. `VectorRowStore` remains available as a simple
in-memory implementation for focused tests or future comparisons.

`BTreeIndex` keeps the existing ordered lookup API while maintaining `BTreeNode` layout metadata
with page ids, leaf links, root children, separator keys, and row-id payloads in leaves. Lookup and
range reads use the leaf payloads; ordered entries remain as the mutation staging structure that
keeps node rebuilds deterministic.

`Table` exposes transaction-aware snapshots through `rowsSnapshot(TransactionId)` and
`rowsById(..., TransactionId)`. The executor routes active-transaction reads through those MVCC
APIs. `BEGIN`/`COMMIT`/`ROLLBACK` use a per-transaction undo log: DML records compensating actions,
`ROLLBACK` applies them LIFO on the same `Database` instance, and `COMMIT` discards the log.

## Current Limitations

- Database snapshots persist typed sparse rows rather than raw page files on disk.
- `BTreeIndex` uses leaf payloads for reads but rebuilds a shallow layout lazily on read rather than
  splitting and merging nodes incrementally.
- WAL recovery is logical SQL replay, not physical page redo.
- Transactions have MVCC read routing and undo-log DML rollback; commit-aware isolation and
  transaction-atomic WAL are still future work.

## Current Data Flow

1. The CLI reads a SQL string.
2. `Tokenizer` emits a token stream.
3. `Parser` creates a strongly typed `Query` variant (including `WITH`, `IN` subqueries, and
   `EXPLAIN`).
4. For `SELECT`/`EXPLAIN`, a rewriter inlines CTEs and the executor materializes `IN` subqueries.
5. `QueryPlanner` chooses an access path (including residual filters) and `QueryExecutor` runs it.
6. Results are returned as `QueryResult` with columns, rows, and a status message.
