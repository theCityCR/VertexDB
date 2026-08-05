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
- CTE index-win `SELECT` (inlined `WITH` + outer equality) with an `id` hash index, at 1,000 and
  100,000 rows.
- Same CTE `SELECT` without an index (full-scan baseline) at 1,000 and 100,000 rows.

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
- CTE index-win path (`BM_CteIndexedWinSelect`) vs. CTE full-scan baseline
  (`BM_CteNonIndexedSelect`) at the same row count — expected shape: indexed stays near point-lookup
  cost; non-indexed grows with table size.
- Single-thread read vs. multi-thread read.
- Debug vs. release builds.
- Sanitized vs. unsanitized builds.

The CTE benchmarks use the wedge query:

```sql
WITH high AS (
  SELECT id, name, salary FROM Employees WHERE salary > 100000.0
)
SELECT name FROM high WHERE id = 1;
```

With `idx_id`, the planner should choose hash index equality on `id` and keep `salary > …` as a
residual after CTE inlining. Without an index, the same SQL is a full scan. See
[cte_index_wedge.md](cte_index_wedge.md).

## Planned Benchmark Work

- Add row-store comparisons between page-backed and vector-backed storage.
- Add B+ tree range-query benchmarks.
- Add transaction read and rollback benchmarks.
- Save periodic benchmark summaries in this document.
