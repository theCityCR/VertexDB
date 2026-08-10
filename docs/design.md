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
- Concurrency: executor-level reader/writer synchronization and concurrent client tests
- Transactions: commit-aware MVCC snapshot isolation, undo-log DML rollback, transaction-batched
  page-image WAL flush on `COMMIT`
- Planner: cost-based access paths (including multi-index AND intersect, top-level OR union with
  partial residual OR, prefix `LIKE`, and trigram intersect), residual filters, join algorithm
  selection (`INNER`/`LEFT`/`RIGHT`/`FULL`/`CROSS`, equi and non-equi), expression-index matching,
  `EXPLAIN`, and `EXPLAIN ANALYZE` (actual vs estimated rows)
- Quality: themed GoogleTest suites, regression tests, sanitizer/coverage scripts, benchmarks with
  a CI CTE cost-shape gate, CI

## Known Limitations

- Schema catalog changes (`CREATE DATABASE`/`TABLE`, `DROP`/`RENAME TABLE`) and save/load are
  rejected inside an open transaction. `CREATE INDEX` is transactional (undo + deferred logical
  WAL); there is no public `DROP INDEX` SQL yet (internal drop supports undo).
- DML WAL redo stores page images (`PageImageRedo`); DDL still uses logical SQL payloads. Legacy
  `PhysicalRedo` and logical `Insert`/`Update`/`Delete` records remain replayable for old WALs.
- Top-level `OR` of equality (or expression-equality) index probes uses multi-index union when the
  indexable subset is cheaper than a full scan. Non-indexable disjuncts become a residual OR
  complementary scan (partial OR). When no disjunct is indexable, or the indexable union is not
  cheaper than a scan, the planner keeps a full scan. Same-column equality `OR` (top-level or under
  `AND`) is rewritten to `IN` for HashIn. Heterogeneous nested `OR` under `AND` may remain as a
  residual while another conjunct uses an index.
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

Quality polish shipped literal `IN (v1, v2, …)` parsing, indexed `UPDATE`/`DELETE` edge-path tests,
expression same-column `OR`→`IN`, and an MVCC fix so `UPDATE` closes the prior version (UPDATE then
`DELETE` no longer resurrects the pre-update image). Multi-index AND intersect is packaged as a second
demo wedge (demo SQL, scaled regression, microbenchmarks, CI shape gate, BitmapAnd parity note).
Catalog DDL and `SAVE`/`LOAD` remain rejected inside open transactions. Heterogeneous nested `OR`
under `AND`, composite Intersect∪Union, and further recursive/set-op surface remain intentionally
limited (see [sql.md](sql.md)). `EXPLAIN ANALYZE` for SELECT/WITH compares `est_rows` to measured
`actual_rows` (and residual `candidates`) in one execute pass. `EXPLAIN` for mutations and public
`DROP INDEX` SQL are still out of scope.

The illustrative absolute-time table in [benchmarks.md](benchmarks.md) was refreshed on 2026-08-10
from the CI `benchmark report` artifact (GHA `ubuntu-latest`). Wedge **cost shape** (CTE: indexed
stays flat, scan grows, materialize ≫ inline; intersect: residual ≫ intersect, residual grows,
intersect growth bounded) is gated on every push/PR via `scripts/run-benchmarks.sh --check-shape`.
Re-refresh the absolute-time summary only after planner or storage changes that make those numbers
stale — prefer `workflow_dispatch` or a commit message containing `[benchmark-report]`, then
`python3 scripts/check_benchmark_shape.py --markdown-table …` (include the new intersect benches
when refreshing).

Demo wedges:

- CTE inlining so outer predicates hit base-table indexes:
  [cte_index_wedge.md](cte_index_wedge.md), [cte_materialize_comparison.md](cte_materialize_comparison.md)
- Multi-index AND intersect vs single-index residual:
  [multi_index_intersect_wedge.md](multi_index_intersect_wedge.md),
  [bitmap_and_comparison.md](bitmap_and_comparison.md)

## Definition of Done

Each feature should include:

- Public interface and implementation.
- Unit or execution tests for desired behavior and edge cases.
- Regression tests for bugs and non-obvious failure modes.
- SQL documentation updates when syntax or behavior changes.
- Benchmark updates when performance-sensitive paths change.
- Clean `ctest`, sanitizer, and coverage runs before merging.
