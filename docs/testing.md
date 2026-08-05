# Testing Strategy

VertexDB uses three levels of automated testing:

- Unit tests for parser, storage, indexes, persistence primitives, planner, and transaction helpers.
- Execution tests for end-to-end SQL behavior through `QueryExecutor`.
- Regression tests for previously fragile behavior that should never silently break.

## Current Coverage

Aim for at least 85% line coverage on the core library. For code that touches persistence,
transactions, indexing, recovery, or concurrency, prefer branch-oriented tests over only increasing
line coverage.

The current suite contains 50 discovered GoogleTest cases across parser, storage, index, execution,
deep feature, and regression tests. The latest local coverage run reported 85.77% line coverage.

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
cmake -S . -B build-benchmark -DVERTEXDB_BUILD_TESTS=OFF -DVERTEXDB_BUILD_BENCHMARKS=ON
cmake --build build-benchmark
```

Use sanitizer and coverage builds before changing shared systems such as `Table`, `QueryExecutor`,
`StorageManager`, indexes, or transaction code. 
