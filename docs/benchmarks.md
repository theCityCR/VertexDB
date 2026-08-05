# Benchmarks

Benchmarks are implemented with Google Benchmark in `benchmarks/storage_benchmarks.cpp`. They catch
broad performance regressions and compare storage and access paths under controlled workloads.

## Current Metrics

- Insert throughput for 1,000 and 100,000 rows.
- Indexed point lookup over 1,000 and 100,000 rows.
- Indexed filtered `SELECT`.
- Non-indexed filtered `SELECT`.
- Update throughput.
- Delete throughput.
- Concurrent indexed point lookup scaling.

Build the benchmark target with:

```sh
cmake -S . -B build-benchmark -DVERTEXDB_BUILD_TESTS=OFF -DVERTEXDB_BUILD_BENCHMARKS=ON
cmake --build build-benchmark
```

Run it with:

```sh
./build-benchmark/VertexDB_benchmarks
```

## Reporting

Benchmark output should be checked into documentation only as summarized tables or generated graphs, not raw build artifacts.

Suggested comparisons:

- Indexed vs. non-indexed lookup.
- Single-thread read vs. multi-thread read.
- Debug vs. release builds.
- Sanitized vs. unsanitized builds.

## Planned Benchmark Work

- Add row-store comparisons between page-backed and vector-backed storage.
- Add B+ tree range-query benchmarks.
- Add transaction read and rollback benchmarks.
- Save periodic benchmark summaries in this document.
