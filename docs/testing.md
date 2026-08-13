# Testing Strategy

VertexDB uses three levels of automated testing:

- Unit tests for parser, storage, indexes, persistence primitives, planner, and transaction helpers.
- Execution tests for end-to-end SQL behavior through `QueryExecutor`.
- Regression tests for previously fragile behavior that should never silently break.

## Test File Ownership

| File | Concern |
| --- | --- |
| `test_support.hpp` / `test_support.cpp` | Shared `makeTempExecutor` / `makeTempRoot` / `seedEmployees` |
| `parser_tests.cpp` | Tokenization, AST grammar (`INNER`/`LEFT`/`RIGHT`/`FULL`/`CROSS`, `LIKE`/`~`, literal `IN` lists vs `IN` subquery, nested `WITH` depth), `EXPLAIN` / `EXPLAIN ANALYZE` / `EXPLAIN INSERT`, `DROP DATABASE`, `ParseError` source positions, CROSS-with-ON / bushy-join refusals |
| `storage_tests.cpp` | Row stores, buffer pool, schema |
| `index_tests.cpp` | Hash / B+ tree unit behavior, expression/`trigram` index metadata |
| `execution_tests.cpp` | End-to-end DML/SELECT smoke, `LEFT`/`RIGHT`/`FULL`/`CROSS`/non-equi joins, `LIKE`/trigram/NULL edges, concurrency, `DROP DATABASE` |
| `nested_cte_tests.cpp` | CTE/derived inlining, materialize modes, `WITH` nesting depth (≤6), CTE join targets, scaled CTE index win |
| `nested_correlation_tests.cpp` | Correlation (≤8 frames, NULL outer keys), FROM/JOIN table aliases for qualification/scopes |
| `nested_subquery_tests.cpp` | `WITH`/`JOIN`/derived inside `IN`/`EXISTS`, uncorrelated `IN`, nested-SQL documented grammar refusals |
| `nested_recursive_tests.cpp` | `WITH RECURSIVE` (walk, multiple independent / mutual recursive CTEs, `AS ACCUMULATOR`, documented refusals, iteration/row caps) |
| `set_ops_tests.cpp` | `UNION` / `UNION ALL` / `INTERSECT` / `INTERSECT ALL` / `EXCEPT` / `EXCEPT ALL`, recursive `UNION` cycle dedup, set-op CTEs / `IN` / `EXPLAIN` |
| `planner_access_tests.cpp` | Access paths (prefix `LIKE` / trigram / regex residual / OR→IN), residuals, expression indexes, histograms |
| `planner_intersect_union_tests.cpp` | Multi-index AND intersect / OR union (incl. partial OR, composite Intersect∪Union for nested OR including partial nested OR under AND, scaled intersect wedge) |
| `planner_explain_tests.cpp` | `EXPLAIN` / `EXPLAIN ANALYZE` actuals (residual `candidates`, joins, aggregation, `LIMIT`, `WITH`) |
| `planner_mutation_tests.cpp` | Indexed UPDATE/DELETE `WHERE` (HashEq/HashIn/range/intersect/prefix LIKE/collect-then-mutate); `EXPLAIN UPDATE`/`DELETE`/`INSERT` |
| `planner_join_stats_tests.cpp` | Stats-driven cost/join choice, outer/`CROSS` join plans |
| `planner_test_support.hpp` | Shared planner stubs (`StubRelationStats` / `StubIndexCatalog`) + temp executor helper |
| `transaction_behavior_tests.cpp` | BEGIN/COMMIT/ROLLBACK (incl. invalid state), MVCC visibility (incl. UPDATE-then-DELETE), SI/SSI anomaly wedge (dirty read, watermark, mid-txn phantom, insert-phantom / empty-probe / update-into-predicate abort, write-skew / write–write abort, executor RW honesty), deferred WAL (incl. durable COMMIT before return), transactional catalog DDL (`CREATE`/`DROP`/`RENAME TABLE`, `CREATE DATABASE`, `CREATE`/`DROP INDEX`), `SAVE`/`LOAD` implicit commit/rollback in open txns |
| `persistence_behavior_tests.cpp` | Page store, snapshot v4/v5, page-image/physical/torn WAL, durable WAL append/`reset`, durable `SAVE` publish (flush+fsync) |
| `aggregate_prepared_tests.cpp` | Aggregates/`GROUP BY` (incl. NULL group keys), prepared statements (AST immutability, `EXPLAIN` / `EXPLAIN ANALYZE` bind), `ANALYZE` |
| `constraint_behavior_tests.cpp` | Single-column `PRIMARY KEY` / `UNIQUE` / `NOT NULL` parse + DML rejection, auto HashEq indexes, DROP INDEX refusal, save/load v5 flags |
| `deep_feature_tests.cpp` | Cross-cutting deep-feature coverage (incl. legacy `.tcrdb` v1–v3 load) |
| `regression_tests.cpp` | Bug fixes and non-obvious failure modes |
| `benchmark_shape/*` | CTE + multi-index-intersect cost-shape + markdown-table fixtures (`scripts/check_benchmark_shape.py --self-test`) |

## Current Coverage

Aim for at least 85% line coverage on the core library. For code that touches persistence,
transactions, indexing, recovery, or concurrency, prefer branch-oriented tests over only increasing
line coverage.

The current suite contains 302 discovered GoogleTest cases across the files above.
The latest local coverage run reported 85.19% line coverage.

`scripts/run-coverage.sh` enforces the 85% default threshold after running the coverage-instrumented
test binary. Override it for local experiments with:

```sh
VERTEXDB_COVERAGE_MIN=90 scripts/run-coverage.sh
```

## Regression Test Policy

Add a regression test whenever:

- a bug is fixed,
- a feature has non-obvious failure modes,
- persistence or WAL behavior changes,
- invalid input must not partially mutate state,
- concurrency or transaction behavior changes,
- SQL grammar behavior is extended.

Regression tests live in `tests/regression_tests.cpp`. Each test should name the behavior it
protects, set up its own isolated temporary storage root when persistence is involved, and assert
both the visible query result and any important internal invariant such as WAL record count.

## Local Verification

```sh
cmake --build build
ctest --test-dir build --output-on-failure
scripts/run-sanitizers.sh
scripts/run-coverage.sh
python3 scripts/check_benchmark_shape.py --self-test
scripts/run-benchmarks.sh --check-shape
```

`scripts/check_benchmark_shape.py --self-test` exercises JSON fixtures under `tests/benchmark_shape/`
(desired pass/fail ratios, median vs iteration fallback, missing CTE benches, illustrative markdown
table). CI also runs Release CTE microbenchmarks and fails if the cost *shape* regresses; see
[benchmarks.md](benchmarks.md). A full-report JSON for doc refresh is a separate `benchmark report`
CI job (`workflow_dispatch` or a `[benchmark-report]` commit), not every push/PR.