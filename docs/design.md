# Design Status

VertexDB is built as small, testable systems slices. The codebase includes working storage, parsing,
execution, indexing, persistence, WAL recovery, transactions, tests, benchmarks, and CI.

## What Exists Today

- Repository foundation: CMake targets, CLI, library target, GitHub Actions CI, and documentation
- Storage engine: typed columns, nullable values, schema validation, table/database ownership,
  page-backed `RowStore`, `VectorRowStore`, `BufferPool`, index maintenance, MVCC version
  recording, and stable row IDs with tombstones plus free-list reuse
- Parser: tokenizer, AST, grammar tests, table-management commands, predicates, ordering, limits,
  joins, `WITH` CTEs, `IN` subqueries, `EXPLAIN`, transactions, prepared statements, save/load, and
  exit
- Query execution: projection, filtering, ordering, limit, insert, update, delete, table
  management, joins, CTE inlining, `IN` subquery materialization, prepared execution, save/load,
  recovery, and transactional read routing
- Indexes: maintained hash indexes for equality lookup and ordered B+ tree index APIs for point
  and range lookup, plus hash index `IN` multi-lookup
- Persistence: versioned binary snapshots for database schemas, sparse row IDs, free-list state,
  and index definitions
- WAL and recovery: append-only logical WAL, startup replay, save checkpoints, and recovery tests
- Concurrency: executor-level reader/writer synchronization and concurrent client tests
- Transactions: transaction manager, transaction states, per-transaction undo-log rollback for DML,
  MVCC row-version store, and active-transaction reads routed through MVCC table APIs
- Planner: sargable conjunct extraction from `AND` trees, residual filters, CTE rewrite notes, and
  `EXPLAIN` text
- Quality: GoogleTest coverage, regression tests, sanitizer script, coverage script, benchmark
  target, and multi-platform CI

## Current Architecture Choices

- `Table` owns schema validation and index maintenance, while row storage is delegated to the
  `RowStore` interface.
- `PageRowStore` is the default row store. It keeps serialized page payloads in an in-memory page
  directory as the source of truth and serves reads through the LRU `BufferPool` (fill-on-miss),
  deserializing row slots from page bytes.
- `VectorRowStore` remains available as a simple in-memory implementation for focused tests and
  comparisons.
- Hash indexes provide fast equality lookup. `BTreeIndex` provides ordered lookup APIs and keeps
  explicit node/page metadata, rebuilding its shallow layout from ordered entries lazily on read.
- The executor uses a rule-based planner that extracts one sargable conjunct from `AND` trees and
  selects a full scan, hash index equality lookup, ordered index range lookup, or hash index `IN`
  lookup, with residual filters for remaining conjuncts.
- `WITH` CTEs are always inlined before planning so outer predicates can hit base-table indexes.
  `IN (SELECT …)` subqueries are planned/executed for their values, then probed via index when
  possible.
- Persistence uses versioned binary snapshots that store capacity, free-list order, and live
  `(rowId, row)` entries. WAL recovery replays logical SQL payloads after the latest save
  checkpoint.
- Transactions use transaction state tracking, MVCC read APIs, and an undo log that reverses DML on
  `ROLLBACK` against the live database (no full-database clone). Schema changes and save/load are
  rejected while a transaction is active. This leaves room for commit-aware MVCC visibility and
  transactional WAL later.

## Known Limitations

- Database snapshots still serialize typed sparse rows rather than persisting raw page files.
- B+ tree insert/delete operations rebuild node layout rather than incrementally splitting and
  merging pages.
- Transaction rollback uses undo records for DML only; commit-aware MVCC isolation and
  transaction-atomic WAL are not implemented yet.
- Schema changes, index creation, and save/load are rejected inside an open transaction.
- WAL records are logical and replay SQL operations; there is no physical redo log yet.
- The planner does not collect statistics or perform cost-based multi-index optimization.
- `OR` predicates are not split for indexing; they force a full scan.
- Nested SQL is limited: no derived tables, no correlated subqueries, no `JOIN` / nested `WITH`
  inside CTE bodies or `IN` subqueries, and no expression/regex indexes.
- SQL support is intentionally limited and does not include aggregation, grouping, or general DDL.

## Next Engineering Plan

1. Add commit-aware MVCC visibility / richer isolation on top of undo-log rollback.
2. Implement incremental B+ tree page split/merge logic with invariants tests.
3. Harden recovery with physical redo records and crash-simulation regression tests.
4. Add statistics to tables and indexes, then evolve the planner toward a cost model.
5. Expand nested SQL (derived tables, correlation) and add expression indexes where useful.
6. Expand SQL support with aggregates, `GROUP BY`, and more join strategies.
7. Turn benchmark output into documented reports and trend comparisons.

### CTE index wedge (parallel track) — first milestone shipped

CTE inlining so outer predicates hit base-table indexes is implemented and packaged: demo SQL,
scaled regression, microbenchmarks, and a Postgres materialize comparison. See the Demo section in
[cte_index_wedge.md](cte_index_wedge.md).

## Definition of Done

Each feature should include:

- Public interface and implementation.
- Unit or execution tests for expected behavior and edge cases.
- Regression tests for bugs and non-obvious failure modes.
- SQL documentation updates when syntax or behavior changes.
- Benchmark updates when performance-sensitive paths change.
- Clean `ctest`, sanitizer, and coverage runs before merging.
