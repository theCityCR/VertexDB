# Design Status

VertexDB is built as small, testable systems slices. The codebase includes working storage, parsing,
execution, indexing, persistence, WAL recovery, transactions, tests, benchmarks, and CI.

For module layout and data flow, see [architecture.md](architecture.md). For mechanism depth (B+ tree,
WAL, MVCC, planner costs), see [deep_features.md](deep_features.md).

## What Exists Today

- Repository foundation: CMake targets, CLI, library target, GitHub Actions CI, and documentation
- Storage engine: typed columns, nullable values, schema validation, table/database ownership,
  page-backed `RowStore`, `VectorRowStore`, `BufferPool`, index maintenance, MVCC version
  recording, and stable row IDs with tombstones plus free-list reuse
- Parser: tokenizer (token offsets / line / column), AST, grammar tests, table-management commands,
  predicates (including `LIKE` and regex `~`), ordering, limits, left-deep `INNER` / `LEFT` / `RIGHT` / `FULL` `[OUTER]` and `CROSS`
  joins with `ON col op col` (`=`, `<`, `>`; none for `CROSS`) and optional join-table aliases, aggregates/`GROUP BY`,
  `WITH` CTEs (`AS MATERIALIZED` / `AS NOT MATERIALIZED`, nesting depth up to 3, minimal
  `WITH RECURSIVE` with `UNION ALL`), derived tables,
  `FROM` / `JOIN` table aliases, `IN`/`EXISTS` subqueries (including `WITH` / `JOIN` inside them and
  correlation through four outer frames), outer `JOIN` against CTE/derived aliases (force
  materialize), expression indexes (including `trigram(column)`),
  `EXPLAIN` / `EXPLAIN ANALYZE`, transactions, prepared statements (typed AST + `?` slots), save/load, exit, and
  `ParseError` diagnostics with source positions
- Query execution: projection, filtering, ordering, limit, aggregates/`GROUP BY`, insert, update,
  delete (UPDATE/DELETE `WHERE` uses the same planner index access paths as SELECT), table management,
  multi-join chains (`INNER`/`LEFT`/`RIGHT`/`FULL`/`CROSS`, equi and non-equi), CTE/derived-table
  inlining or materialization (including recursive delta iteration), correlated `IN`/`EXISTS` with
  alias scopes (including joined subqueries), expression-index maintenance (including trigram),
  prepared AST binding, save/load, recovery, and transactional read routing
- Indexes: maintained hash indexes for equality lookup and ordered B+ tree index APIs for point
  and range lookup (column and expression keys), hash index `IN` multi-lookup, ordered prefix
  `LIKE`, and hash trigram indexes for substring `LIKE`
- Persistence: versioned binary snapshots (current page-payload + index-pages v4; page-payload v3,
  sparse v2, and dense v1 still readable) under `.tcrdb` files, with `tcrdb_codec` owning the layout
- WAL and recovery: append-only WAL with page-image redo for DML (legacy physical row-image redo
  still replayable), logical SQL for DDL, truncated-trailing-record tolerance, startup replay, save
  checkpoints, and crash-simulation tests
- Concurrency: executor-level reader/writer synchronization (`LockManager`) and concurrent client
  tests; SI anomaly evidence packaged in [si_anomaly_wedge.md](si_anomaly_wedge.md)
- Transactions: commit-aware MVCC snapshot isolation (dirty reads / SI watermark / mid-txn phantoms
  prevented; write skew allowed), undo-log DML rollback, transaction-batched page-image WAL flush
  on `COMMIT`
- Planner: cost-based access paths (including multi-index AND intersect, top-level OR union with
  partial residual OR, composite Intersect∪Union for fully indexable nested OR under AND, prefix
  `LIKE`, and trigram intersect), residual filters, join algorithm
  selection (`INNER`/`LEFT`/`RIGHT`/`FULL`/`CROSS`, equi and non-equi), expression-index matching,
  `EXPLAIN`, and `EXPLAIN ANALYZE` (actual vs estimated rows)
- Quality: themed GoogleTest suites, regression tests, sanitizer/coverage scripts, benchmarks with
  a CI CTE cost-shape gate, CI

## Known Limitations

- Snapshot isolation prevents dirty reads and hides commits after `BEGIN`; classic SI still allows
  write skew. There are no row/page locks, predicate locks, or SSI aborts. See
  [si_anomaly_wedge.md](si_anomaly_wedge.md).
- `SAVE DATABASE` and `LOAD DATABASE` are still rejected inside an open transaction.
- DML WAL redo stores page images (`PageImageRedo`); DDL still uses logical SQL payloads. Legacy
  `PhysicalRedo` and logical `Insert`/`Update`/`Delete` records remain replayable for old WALs.
- Top-level `OR` of equality (or expression-equality) index probes uses multi-index union when the
  indexable subset is cheaper than a full scan. Non-indexable disjuncts become a residual OR
  complementary scan (partial OR). When no disjunct is indexable, or the indexable union is not
  cheaper than a scan, the planner keeps a full scan. Same-column equality `OR` (top-level or under
  `AND`) is rewritten to `IN` for HashIn. A heterogeneous `OR` nested under `AND` whose every
  disjunct is an equality index probe becomes a Union child of a multi-index Intersect
  (composite Intersect∪Union) when that plan beats the best single conjunct. If any nested-OR arm
  is not equality-indexable, the whole `OrPred` stays an AND residual.
- Nested SQL is limited: `WITH` nesting deeper than depth 3 and correlation deeper than four outer
  frames are rejected. Supported nested forms include `WITH` nesting depth up to 3, `WITH` /
  derived tables and `JOIN` inside `IN`/`EXISTS`, outer `JOIN` against a CTE/derived alias (force
  materialize), minimal `WITH RECURSIVE` (`UNION ALL`, delta self-ref, 1000-iteration /
  100000-row caps), `FROM` / `JOIN` table aliases (`AS` optional) for qualification and
  correlation scopes, and expression indexes (`column`, `-column`, `column+/-literal`,
  `trigram(column)`). CTE/derived bodies may include left-deep `INNER` / `LEFT` / `RIGHT` /
  `FULL` / `CROSS` joins. Parser/tokenizer failures report `line`/`column` source positions via
  `ParseError`.
- Aggregates/`GROUP BY` are supported; joins are left-deep `INNER` / `LEFT` / `RIGHT` / `FULL`
  `[OUTER]` and `CROSS` chains (`ON col op col` for non-`CROSS`). General DDL beyond
  the current table/index commands is still out of scope.

## Next Steps

Shipped recently (no longer open work): literal `IN` lists, indexed `UPDATE`/`DELETE` access paths,
transactional `CREATE INDEX` / `DROP INDEX`, same-column `OR`→`IN`, MVCC `UPDATE` close-prior-version,
`EXPLAIN ANALYZE` (actual vs estimated rows for SELECT/WITH), `EXPLAIN UPDATE`/`DELETE`, multi-index
AND intersect packaging, the SI anomaly concurrency wedge, and composite Intersect∪Union for fully
indexable nested `OR` under `AND`.

Forward-looking options (intentional gaps, pick by teaching value):

- SQL / catalog: `SAVE`/`LOAD` inside open transactions
- Recursive / set-ops beyond minimal `WITH RECURSIVE … UNION ALL` (see [sql.md](sql.md))
- Maintenance: re-refresh absolute times in [benchmarks.md](benchmarks.md) after planner/storage
  changes that stale the 2026-08-10 table (include intersect benches); wedge **cost shape** already
  gates every push/PR via `scripts/run-benchmarks.sh --check-shape`
- Planner follow-ups: partial nested `OR` under `AND` (indexable arms + complementary residual
  under the outer AND) without expanding to full SSI-style boolean algebra

Demo wedges (done):

- CTE inlining so outer predicates hit base-table indexes:
  [cte_index_wedge.md](cte_index_wedge.md), [cte_materialize_comparison.md](cte_materialize_comparison.md)
- Multi-index AND intersect vs single-index residual:
  [multi_index_intersect_wedge.md](multi_index_intersect_wedge.md),
  [bitmap_and_comparison.md](bitmap_and_comparison.md)
- Snapshot isolation anomalies (prevented vs allowed) and executor RW honesty:
  [si_anomaly_wedge.md](si_anomaly_wedge.md)

## Definition of Done

Each feature should include:

- Public interface and implementation.
- Unit or execution tests for desired behavior and edge cases.
- Regression tests for bugs and non-obvious failure modes.
- SQL documentation updates when syntax or behavior changes.
- Benchmark updates when performance-sensitive paths change.
- Clean `ctest`, sanitizer, and coverage runs before merging.
