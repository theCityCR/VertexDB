# Design Status

VertexDB is built as small, testable systems slices. The codebase includes working storage, parsing,
execution, indexing, persistence, WAL recovery, transactions, tests, benchmarks, and CI.

## What Exists Today

- Repository foundation: CMake targets, CLI, library target, GitHub Actions CI, and documentation
- Storage engine: typed columns, nullable values, schema validation, table/database ownership,
  page-backed `RowStore`, `VectorRowStore`, `BufferPool`, index maintenance, and MVCC version
  recording
- Parser: tokenizer, AST, grammar tests, table-management commands, predicates, ordering, limits,
  joins, transactions, prepared statements, save/load, and exit
- Query execution: projection, filtering, ordering, limit, insert, update, delete, table
  management, joins, prepared execution, save/load, recovery, and transactional read routing
- Indexes: maintained hash indexes for equality lookup and ordered B+ tree index APIs for point
  and range lookup
- Persistence: versioned binary snapshots for database schemas, rows, and index definitions
- WAL and recovery: append-only logical WAL, startup replay, save checkpoints, and recovery tests
- Concurrency: executor-level reader/writer synchronization and concurrent client tests
- Transactions: transaction manager, transaction states, snapshot rollback, MVCC row-version store,
  and active-transaction reads routed through MVCC table APIs
- Quality: GoogleTest coverage, regression tests, sanitizer script, coverage script, benchmark
  target, and multi-platform CI

## Current Architecture Choices

- `Table` owns schema validation and index maintenance, while row storage is delegated to the
  `RowStore` interface.
- `PageRowStore` is the default row store. It groups rows into logical pages and mirrors serialized
  page bytes through the LRU `BufferPool`.
- `VectorRowStore` remains available as a simple in-memory implementation for focused tests and
  comparisons.
- Hash indexes provide fast equality lookup. `BTreeIndex` provides ordered lookup APIs and keeps
  explicit node/page metadata, but still rebuilds its shallow layout from ordered entries on write.
- The executor uses a rule-based planner that selects a full scan, hash index equality lookup, or
  ordered index range lookup.
- Persistence uses versioned binary snapshots. WAL recovery replays logical SQL payloads after the
  latest save checkpoint.
- Transactions currently combine transaction state tracking, MVCC read APIs, and snapshot-copy
  rollback. This keeps behavior explainable while leaving room for real undo/redo and commit
  visibility.

## Known Limitations

- Row IDs are positional. Deletes compact storage, which can shift later row IDs.
- Page payloads are serialized into the buffer pool, but typed rows remain the operational source
  of truth inside `PageRowStore`.
- B+ tree insert/delete operations rebuild node layout rather than incrementally splitting and
  merging pages.
- Transaction rollback restores a cloned database snapshot rather than applying undo records.
- MVCC does not yet enforce full isolation levels or conflict detection.
- WAL records are logical and replay SQL operations; there is no physical redo log yet.
- The planner does not collect statistics or perform cost-based optimization.
- SQL support is intentionally limited and does not include aggregation, grouping, subqueries, or
  general DDL.

## Next Engineering Plan

1. Introduce stable row IDs with tombstones and free-list reuse.
2. Make `PageRowStore` load rows from page bytes so page storage becomes the source of truth.
3. Replace snapshot rollback with undo records or MVCC commit visibility.
4. Implement incremental B+ tree page split/merge logic with invariants tests.
5. Harden recovery with physical redo records and crash-simulation regression tests.
6. Add statistics to tables and indexes, then evolve the planner toward a cost model.
7. Expand SQL support with aggregates, `GROUP BY`, and more join strategies.
8. Turn benchmark output into documented reports and trend comparisons.

## Definition of Done

Each feature should include:

- Public interface and implementation.
- Unit or execution tests for expected behavior and edge cases.
- Regression tests for bugs and non-obvious failure modes.
- SQL documentation updates when syntax or behavior changes.
- Benchmark updates when performance-sensitive paths change.
- Clean `ctest`, sanitizer, and coverage runs before merging.
