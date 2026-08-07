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

Build the benchmark target with a Release config (numbers below assume `-O3`):

```sh
cmake -S . -B build-benchmark \
  -DVERTEXDB_BUILD_TESTS=OFF \
  -DVERTEXDB_BUILD_BENCHMARKS=ON \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-benchmark
```

Run it with:

```sh
./build-benchmark/VertexDB_benchmarks
# focused report suite (matches the summary table; skips multi-minute Update/Delete):
./build-benchmark/VertexDB_benchmarks \
  --benchmark_filter='BM_IndexedPointLookup|BM_FilteredSelect|BM_NonIndexedFilteredSelect|BM_ConcurrentPointLookups|BM_VectorRowStore|BM_PageRowStore|BM_BTreeRangeQuery|BM_Transaction|BM_Cte' \
  --benchmark_min_time=0.05s
```

## Reporting

Benchmark output should be checked into documentation only as summarized tables or generated graphs, not raw build artifacts.

Suggested comparisons:

- Indexed vs. non-indexed lookup / filtered `SELECT`.
- CTE index-win path (`BM_CteIndexedWinSelect`) vs. CTE full-scan baseline
  (`BM_CteNonIndexedSelect`) at the same row count — expected shape: indexed stays near point-lookup
  cost; non-indexed grows with table size.
- Inlined CTE vs `AS MATERIALIZED` (`BM_CteMaterializedSelect`) — materialize rebuilds a temp each
  iteration and should be far slower than the index-win inline path.
- Page vs vector row-store insert/select.
- Single-thread read vs. multi-thread read (`BM_ConcurrentPointLookups`).
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

## Summary — 2026-08-07

Machine: Apple Silicon host, 12 logical CPUs (Google Benchmark reported 1000 MHz under the agent
sandbox — clock metadata only). Binary: `build-benchmark/VertexDB_benchmarks` with
`CMAKE_BUILD_TYPE=Release` (`-O3 -DNDEBUG`). Filter as above with `--benchmark_min_time=0.05s`.
Times are Google Benchmark **CPU time**. Executor `Update`/`Delete` benches are omitted from this
table (multi‑minute iterations under page-image WAL); they remain in the binary for local runs.

| Benchmark | Arg | CPU time |
| --- | ---: | ---: |
| `BM_IndexedPointLookup` | 1,000 | ~239 ns |
| `BM_IndexedPointLookup` | 100,000 | ~247 ns |
| `BM_FilteredSelect` | 1,000 | ~5.1 µs |
| `BM_FilteredSelect` | 100,000 | ~4.4 µs |
| `BM_NonIndexedFilteredSelect` | 1,000 | ~369 µs |
| `BM_NonIndexedFilteredSelect` | 100,000 | ~36 ms |
| `BM_ConcurrentPointLookups` | 1 | ~38 µs |
| `BM_ConcurrentPointLookups` | 2 | ~70 µs |
| `BM_ConcurrentPointLookups` | 12 | ~343 µs |
| `BM_CteIndexedWinSelect` | 1,000 | ~8.4 µs |
| `BM_CteIndexedWinSelect` | 100,000 | ~7.1 µs |
| `BM_CteNonIndexedSelect` | 1,000 | ~349 µs |
| `BM_CteNonIndexedSelect` | 100,000 | ~85 ms |
| `BM_CteMaterializedSelect` | 1,000 | ~124 ms |
| `BM_CteMaterializedSelect` | 10,000 | ~1.31 s |
| `BM_VectorRowStoreInsert` | 1,000 | ~303 µs |
| `BM_VectorRowStoreInsert` | 100,000 | ~29 ms |
| `BM_PageRowStoreInsert` | 1,000 | ~52 ms |
| `BM_PageRowStoreInsert` | 10,000 | ~576 ms |
| `BM_VectorRowStoreSelect` | 1,000 / 100,000 | ~2.7–3.0 ns |
| `BM_PageRowStoreSelect` | 1,000 / 100,000 | ~13–15 ns |
| `BM_BTreeRangeQuery` | 1,000 | ~13 µs |
| `BM_BTreeRangeQuery` | 100,000 | ~407 µs |
| `BM_TransactionSnapshotRead` | 1,000 | ~3.3 µs |
| `BM_TransactionSnapshotRead` | 10,000 | ~3.6 µs |
| `BM_TransactionRollback` | 100 | ~513 ms |
| `BM_TransactionRollback` | 1,000 | ~5.4 s |

Takeaways from this run:

- Inlined CTE index-win stays near point-lookup cost as N grows (~7–8 µs); the non-indexed CTE
  baseline grows with table size (~85 ms at 100k). `AS MATERIALIZED` pays a large temp-build cost
  each iteration (~10⁴× slower than the index-win inline path here).
- Indexed filtered `SELECT` stays ~5 µs across 1k–100k rows; non-indexed filtered `SELECT` grows
  with N (~36 ms at 100k).
- Concurrent point lookups scale sub-linearly in CPU time as worker count rises (1 → 12).
- Vector row-store appends remain much cheaper than page-store appends; mid-row `get` is ~5× faster
  on the vector store (contiguous vs page deserialize/cache).
- B+ range cost grows with result cardinality (half the table).
- Snapshot reads stay cheap; SQL `INSERT`+`ROLLBACK` undo is expensive relative to direct table
  inserts (executor/WAL buffering path).

## Remaining Follow-ups

- Re-run this Release summary after planner or storage changes that affect the measured paths.
- Optional: Debug vs Release and sanitizer comparison tables.
- Optional: include executor `Update`/`Delete` once those paths are cheap enough for short
  `--benchmark_min_time` report runs.
