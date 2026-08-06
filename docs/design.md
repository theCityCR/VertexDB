# Design Status

VertexDB is built as small, testable systems slices. The codebase includes working storage, parsing,
execution, indexing, persistence, WAL recovery, transactions, tests, benchmarks, and CI.

## What Exists Today

- Repository foundation: CMake targets, CLI, library target, GitHub Actions CI, and documentation
- Storage engine: typed columns, nullable values, schema validation, table/database ownership,
  page-backed `RowStore`, `VectorRowStore`, `BufferPool`, index maintenance, MVCC version
  recording, and stable row IDs with tombstones plus free-list reuse
- Parser: tokenizer, AST, grammar tests, table-management commands, predicates, ordering, limits,
  left-deep equi-joins, aggregates/`GROUP BY`, `WITH` CTEs (`AS MATERIALIZED` / `AS NOT MATERIALIZED`),
  derived tables, `IN`/`EXISTS` subqueries (including single-level correlation), expression indexes,
  `EXPLAIN`, transactions, prepared statements (typed AST + `?` slots), save/load, and exit
- Query execution: projection, filtering, ordering, limit, aggregates/`GROUP BY`, insert, update,
  delete, table management, multi-join chains, CTE/derived-table inlining or materialization,
  correlated `IN`/`EXISTS`, expression-index maintenance, prepared AST binding, save/load, recovery,
  and transactional read routing
- Indexes: maintained hash indexes for equality lookup and ordered B+ tree index APIs for point
  and range lookup (column and expression keys), plus hash index `IN` multi-lookup
- Persistence: versioned binary snapshots (current page-payload + index-pages v4; page-payload v3,
  sparse v2, and dense v1 still readable) for database schemas, page directory bytes, free-list
  state, index definitions (column or expression metadata), and durable B+ tree / hash index pages
  under `.tcrdb` files
- WAL and recovery: append-only WAL with page-image redo for DML (legacy physical row-image redo
  still replayable), logical SQL for DDL, truncated-trailing-record tolerance, startup replay, save
  checkpoints, and crash-simulation tests
- Concurrency: executor-level reader/writer synchronization and concurrent client tests
- Transactions: transaction manager with commit sequences, per-transaction undo-log rollback for DML,
  MVCC row-version store stamped with SQL transaction ids, commit-aware snapshot reads
  (including dirty-read prevention and snapshot isolation across concurrent statements), and
  transaction-batched page-image WAL flush on `COMMIT`
- Planner: cheapest indexable conjunct selection from `AND` trees using row-count and index
  distinct-key statistics, residual filters, CTE rewrite notes (inline vs materialize), join
  algorithm selection (hash vs nested-loop index probe), expression-index matching, and `EXPLAIN`
  text with `est_rows` / `cost`
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
- Hash indexes provide fast equality lookup via `Table::indexedLookup`. `BTreeIndex` provides
  ordered lookup via `Table::orderedLookup` and maintains an incremental B+ tree (leaf/internal
  split and merge) with explicit node/page metadata. `hasIndex` reports whether a column has a
  maintained index (hash + ordered today).
- Execution is split for navigation: `QueryExecutor` dispatches commands; predicate evaluation,
  SELECT helpers, SQL/WAL literals, and recovery live in dedicated translation units.
- The executor uses a cost-based planner that picks the cheapest indexable access path from `AND`
  trees using live row counts, index distinct-key counts, and optional `ANALYZE` histograms
  (equality ≈ \(N/D\), range ≈ histogram selectivity or \(N/3\), `IN` ≈ histogram or \(K\cdot N/D\)).
  Paths include full scan, hash equality, ordered range, hash `IN`, and multi-index equality
  intersect when ≥2 equality probes are cheaper than a single index + residual. Remaining conjuncts
  become residual filters. Top-level `OR` predicates are not split and force a full scan (no index
  union yet); nested `OR` under `AND` may remain as a residual while another conjunct uses an index.
  Equi-joins choose hash join or nested-loop index probe from the same statistics.
- `WITH` CTEs default to inlining (and `AS NOT MATERIALIZED` is explicit inline); `AS MATERIALIZED`
  executes the body into an ephemeral indexed table before planning the outer query. Derived tables
  `FROM (SELECT …) [AS] alias` always inline. `IN (SELECT …)` subqueries are planned/executed for
  their values when uncorrelated; correlated `IN`/`EXISTS` bind a single outer scope per row.
  CTE/derived bodies may include equi-joins (including left-deep multi-join chains). Aggregates and
  `GROUP BY` run after filter/join. Prepared statements store a typed AST with parameter slots.
- Persistence uses versioned binary snapshots (magic `TCRDB001`, current format v4) that store
  `rowsPerPage`, capacity, free-list order, serialized page-directory payloads, and per-index B+ tree
  nodes plus hash buckets. Optional per-column equi-height histogram blobs follow index pages
  (`VDBHIST1` marker; absent on older v4 files). Index definitions encode expression indexes as
  `expr:…` alongside column names. On v4 load, heap pages are restored then index pages are installed
  without `rebuildIndexes()`, and histograms are restored when present. v3 loads page payloads and
  rebuilds indexes; sparse v2 and dense v1 remain readable. WAL recovery replays page-image / legacy
  physical / logical SQL payloads after the latest save checkpoint.
- Transactions use transaction state tracking, MVCC read APIs, and an undo log that reverses DML on
  `ROLLBACK` against the live database (no full-database clone). Version stamps use SQL transaction
  ids; `BEGIN` captures a commit-seq snapshot for isolation. While a transaction is active, the
  executor rejects `CREATE DATABASE`/`TABLE`, `DROP`/`RENAME TABLE`, `CREATE INDEX`, and
  `SAVE`/`LOAD`. DML page-image redo records are deferred until `COMMIT` (flushed as one atomic batch)
  and dropped on `ROLLBACK`.

## Known Limitations

- Schema changes, index creation, and save/load are rejected inside an open transaction.
- DML WAL redo stores page images (`PageImageRedo`); DDL still uses logical SQL payloads. Legacy
  `PhysicalRedo` and logical `Insert`/`Update`/`Delete` records remain replayable for old WALs.
- Planner costs use live \(N\), index distinct keys \(D\), and optional equi-height histograms from
  `ANALYZE` for range/`IN` selectivity. Multi-index AND intersection of equality probes is
  supported when cheaper than a single index + residual. Top-level `OR` index union is not
  implemented yet.
- Top-level `OR` predicates are not split for indexing; they force a full scan.
- Nested SQL is limited: no nested `WITH`, no multi-level correlated subqueries, no outer `JOIN`
  against a CTE/derived alias, no `JOIN` inside `IN`/`EXISTS` subqueries, and no regex/substring
  indexes. Single-level correlation and expression indexes (`column`, `-column`, `column+/-literal`)
  are supported. CTE/derived bodies may include equi-joins.
- Aggregates/`GROUP BY` are supported; joins are left-deep equi-join chains only (no outer/cross
  joins). General DDL beyond the current table/index commands is still out of scope.

## Next Engineering Plan

1. Persist index pages in snapshots; evolve redo toward page images. — **done** (v4 + `PageImageRedo`)
2. Add correlated subqueries, expression indexes, and `WITH … AS MATERIALIZED`. — **done**
3. Expand SQL support with aggregates, `GROUP BY`, and multiple joins. — **done**
4. Add histograms / `ANALYZE` and multi-index AND optimization. — **done**
5. Turn benchmark output into documented reports and trend comparisons. — **done**
   (see [benchmarks.md](benchmarks.md))

### CTE index wedge (parallel track) — first milestone shipped

CTE inlining so outer predicates hit base-table indexes is implemented and packaged: demo SQL,
scaled regression, microbenchmarks, and a Postgres materialize comparison. See the Demo section in
[cte_index_wedge.md](cte_index_wedge.md).

## Definition of Done

Each feature should include:

- Public interface and implementation.
- Unit or execution tests for desired behavior and edge cases.
- Regression tests for bugs and non-obvious failure modes.
- SQL documentation updates when syntax or behavior changes.
- Benchmark updates when performance-sensitive paths change.
- Clean `ctest`, sanitizer, and coverage runs before merging.
