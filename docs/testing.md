# Testing Strategy

VertexDB uses three levels of automated testing:

- Unit tests for parser, storage, indexes, persistence primitives, planner, and transaction helpers.
- Execution tests for end-to-end SQL behavior through `QueryExecutor`.
- Regression tests for previously fragile behavior that should never silently break.

## Test File Ownership

| File | Concern |
| --- | --- |
| `test_support.hpp` / `test_support.cpp` | Shared `makeTempExecutor` / `makeTempRoot` / `seedEmployees` |
| `parser_tests.cpp` | Tokenization, AST grammar, `ParseError` source positions |
| `storage_tests.cpp` | Row stores, buffer pool, schema |
| `index_tests.cpp` | Hash / B+ tree unit behavior |
| `execution_tests.cpp` | End-to-end DML/SELECT smoke and concurrency |
| `nested_sql_tests.cpp` | CTE/derived inlining, nested `WITH`, FROM/JOIN aliases, `WITH` in `IN`/`EXISTS`, correlation (≤4 frames), refusals |
| `planner_behavior_tests.cpp` | Access paths, residuals, stats/cost, multi-index intersect/union, `EXPLAIN` |
| `transaction_behavior_tests.cpp` | BEGIN/COMMIT/ROLLBACK, MVCC visibility, deferred WAL |
| `persistence_behavior_tests.cpp` | Page store, snapshot v4, page-image/physical/torn WAL |
| `aggregate_prepared_tests.cpp` | Aggregates/`GROUP BY`, prepared statements, `ANALYZE` |
| `deep_feature_tests.cpp` | Cross-cutting deep-feature coverage |
| `regression_tests.cpp` | Bug fixes and non-obvious failure modes |
| `benchmark_shape/*` | CTE cost-shape + markdown-table fixtures (`scripts/check_benchmark_shape.py --self-test`) |

## Current Coverage

Aim for at least 85% line coverage on the core library. For code that touches persistence,
transactions, indexing, recovery, or concurrency, prefer branch-oriented tests over only increasing
line coverage.

The current suite contains 179 discovered GoogleTest cases across the files above.
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