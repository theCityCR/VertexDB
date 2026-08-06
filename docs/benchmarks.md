# Benchmarks

Benchmarks are implemented with Google Benchmark in `benchmarks/storage_benchmarks.cpp`. They catch
broad performance regressions and compare storage and access paths under controlled workloads.

**Important:** every `QueryExecutor` bench uses an isolated temp storage root (never the default
`./data` path). Sharing one WAL across iterations can grow multi‑GB files and starve the host.

## Current Metrics

- Insert throughput for 1,000 and 100,000 rows.
- Indexed point lookup over 1,000 and 100,000 rows.
- Indexed filtered `SELECT`.
- Non-indexed filtered `SELECT`.
- Update throughput (1,000 and 10,000 rows).
- Delete throughput (1,000 and 10,000 rows).
- Concurrent indexed point lookup scaling.
- CTE index-win `SELECT` (inlined `WITH` + outer equality) with an `id` hash index, at 1,000 and
  100,000 rows.
- Same CTE `SELECT` without an index (full-scan baseline) at 1,000 and 100,000 rows.
- Page-backed vs vector-backed row-store insert/select.
- B+ tree ordered range lookup (`id > mid`).
- Transaction snapshot read (`BEGIN` + indexed `SELECT`) and `BEGIN`/`ROLLBACK` undo path.
- MATERIALIZED CTE select vs inlined CTE index-win (same outer filter).

Build the benchmark target with:

```sh
cmake -S . -B build-benchmark -DVERTEXDB_BUILD_TESTS=OFF -DVERTEXDB_BUILD_BENCHMARKS=ON
cmake --build build-benchmark
```

Run it with:

```sh
./build-benchmark/VertexDB_benchmarks
# or a focused, short run:
./build-benchmark/VertexDB_benchmarks \
  --benchmark_filter='BM_VectorRowStore|BM_PageRowStore|BM_BTreeRangeQuery|BM_Transaction|BM_Cte' \
  --benchmark_min_time=0.05s
```

## Reporting

Benchmark output should be checked into documentation only as summarized tables or generated graphs, not raw build artifacts.

Suggested comparisons:

- Indexed vs. non-indexed lookup.
- CTE index-win path (`BM_CteIndexedWinSelect`) vs. CTE full-scan baseline
  (`BM_CteNonIndexedSelect`) at the same row count — expected shape: indexed stays near point-lookup
  cost; non-indexed grows with table size.
- Inlined CTE vs `AS MATERIALIZED` (`BM_CteMaterializedSelect`) — materialize rebuilds a temp each
  iteration and should be far slower than the index-win inline path.
- Page vs vector row-store insert/select.
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
[cte_index_wedge.md](cte_index_wedge.md) and the external Postgres comparison in
[cte_materialize_comparison.md](cte_materialize_comparison.md).

## Summary — 2026-08-06

Machine: Apple Silicon host, 12 logical CPUs (Google Benchmark reported 1000 MHz under sandbox —
clock metadata only). Binary: `build-benchmark/VertexDB_benchmarks` (Release-ish local CMake
default). Filter runs with `--benchmark_min_time=0.05s`. Times are Google Benchmark **CPU time**.

| Benchmark | Arg | CPU time |
| --- | ---: | ---: |
| `BM_CteIndexedWinSelect` | 1,000 | ~4.5 µs |
| `BM_CteIndexedWinSelect` | 100,000 | ~5.9 µs |
| `BM_CteMaterializedSelect` | 1,000 | ~117 ms |
| `BM_CteMaterializedSelect` | 10,000 | ~971 ms |
| `BM_VectorRowStoreInsert` | 1,000 | ~181 µs |
| `BM_VectorRowStoreInsert` | 100,000 | ~19 ms |
| `BM_PageRowStoreInsert` | 1,000 | ~31 ms |
| `BM_PageRowStoreInsert` | 10,000 | ~426 ms |
| `BM_VectorRowStoreSelect` | 1,000 / 100,000 | ~2.0 ns |
| `BM_PageRowStoreSelect` | 1,000 / 100,000 | ~10.5 ns |
| `BM_BTreeRangeQuery` | 1,000 | ~4.1 µs |
| `BM_BTreeRangeQuery` | 100,000 | ~1.9 ms |
| `BM_TransactionSnapshotRead` | 1,000 | ~2.4 µs |
| `BM_TransactionSnapshotRead` | 10,000 | ~3.2 µs |
| `BM_TransactionRollback` | 100 | ~474 ms |
| `BM_TransactionRollback` | 1,000 | ~4.9 s |

Takeaways from this run:

- Inlined CTE index-win stays near point-lookup cost as N grows; `AS MATERIALIZED` pays a large
  temp-build cost each iteration (~10⁴–10⁵× slower here).
- Vector row-store appends are much cheaper than page-store appends; mid-row `get` is ~5× faster
  on the vector store (contiguous vs page deserialize/cache).
- B+ range cost grows with result cardinality (half the table).
- Snapshot reads stay cheap; SQL `INSERT`+`ROLLBACK` undo is expensive relative to direct table
  inserts (executor/WAL buffering path).

## Remaining Follow-ups

- Periodic re-runs on a fixed Release config (document `CMAKE_BUILD_TYPE=Release` in the table).
- Optional: Debug vs Release and sanitizer comparison tables.
- Optional: concurrent lookup scaling row in the summary table.
