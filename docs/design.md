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
- Parser: tokenizer, AST, grammar tests, table-management commands, predicates, ordering, limits,
  left-deep equi-joins, aggregates/`GROUP BY`, `WITH` CTEs (`AS MATERIALIZED` / `AS NOT MATERIALIZED`,
  one level of nested `WITH`), derived tables, `FROM` table aliases, `IN`/`EXISTS` subqueries
  (including `WITH` inside them and two-level correlation), expression indexes, `EXPLAIN`,
  transactions, prepared statements (typed AST + `?` slots), save/load, and exit
- Query execution: projection, filtering, ordering, limit, aggregates/`GROUP BY`, insert, update,
  delete, table management, multi-join chains, CTE/derived-table inlining or materialization,
  correlated `IN`/`EXISTS` with alias scopes, expression-index maintenance, prepared AST binding,
  save/load, recovery, and transactional read routing
- Indexes: maintained hash indexes for equality lookup and ordered B+ tree index APIs for point
  and range lookup (column and expression keys), plus hash index `IN` multi-lookup
- Persistence: versioned binary snapshots (current page-payload + index-pages v4; page-payload v3,
  sparse v2, and dense v1 still readable) under `.tcrdb` files, with `tcrdb_codec` owning the layout
- WAL and recovery: append-only WAL with page-image redo for DML (legacy physical row-image redo
  still replayable), logical SQL for DDL, truncated-trailing-record tolerance, startup replay, save
  checkpoints, and crash-simulation tests
- Concurrency: executor-level reader/writer synchronization and concurrent client tests
- Transactions: commit-aware MVCC snapshot isolation, undo-log DML rollback, transaction-batched
  page-image WAL flush on `COMMIT`
- Planner: cost-based access paths (including multi-index AND intersect and top-level OR union with
  partial residual OR), residual filters, join algorithm selection, expression-index matching, and
  `EXPLAIN`
- Quality: themed GoogleTest suites, regression tests, sanitizer/coverage scripts, benchmarks with
  a CI CTE cost-shape gate, CI

## Known Limitations

- Schema changes, index creation, and save/load are rejected inside an open transaction.
- DML WAL redo stores page images (`PageImageRedo`); DDL still uses logical SQL payloads. Legacy
  `PhysicalRedo` and logical `Insert`/`Update`/`Delete` records remain replayable for old WALs.
- Top-level `OR` of equality (or expression-equality) index probes uses multi-index union when the
  indexable subset is cheaper than a full scan. Non-indexable disjuncts become a residual OR
  complementary scan (partial OR). When no disjunct is indexable, or the indexable union is not
  cheaper than a scan, the planner keeps a full scan. Nested `OR` under `AND` may remain as a
  residual while another conjunct uses an index.
- Nested SQL is limited: nested `WITH` deeper than one level, correlation deeper than two outer
  frames, outer `JOIN` against a CTE/derived alias, `JOIN` inside `IN`/`EXISTS` subqueries, and
  regex/substring indexes are unsupported. Supported nested forms include one nested `WITH`,
  `WITH` / derived tables inside `IN`/`EXISTS`, `FROM` table aliases (`AS` optional) for correlation
  scopes, two-level correlated `IN`/`EXISTS`, and expression indexes (`column`, `-column`,
  `column+/-literal`). CTE/derived bodies may include equi-joins.
- Aggregates/`GROUP BY` are supported; joins are left-deep equi-join chains only (no outer/cross
  joins). General DDL beyond the current table/index commands is still out of scope.

## Next Steps

1. Optional: refresh the illustrative absolute-time table in [benchmarks.md](benchmarks.md) after
   planner or storage changes. CTE **cost shape** (indexed stays flat, scan grows, materialize ≫
   inline) is already gated in CI via `scripts/run-benchmarks.sh --check-shape`.
2. Optional nested-SQL polish still in educational scope: join-table aliases, clearer diagnostics
   with source positions, or correlation deeper than two frames.

CTE inlining so outer predicates hit base-table indexes is packaged as a demo wedge: see
[cte_index_wedge.md](cte_index_wedge.md) and [cte_materialize_comparison.md](cte_materialize_comparison.md).

## Definition of Done

Each feature should include:

- Public interface and implementation.
- Unit or execution tests for desired behavior and edge cases.
- Regression tests for bugs and non-obvious failure modes.
- SQL documentation updates when syntax or behavior changes.
- Benchmark updates when performance-sensitive paths change.
- Clean `ctest`, sanitizer, and coverage runs before merging.
