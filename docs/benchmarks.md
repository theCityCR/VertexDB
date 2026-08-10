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

Build and run via [`scripts/run-benchmarks.sh`](../scripts/run-benchmarks.sh) (Release `-O3`, median of
5 repetitions, `--benchmark_min_time=0.5s`, random interleaving). That script writes Google Benchmark
JSON and then checks **CTE cost-shape ratios** from the same process (CPU time, not wall time):

```sh
scripts/run-benchmarks.sh                 # full report suite + shape check
scripts/run-benchmarks.sh --check-shape   # CTE benches only (CI gate)
python3 scripts/check_benchmark_shape.py --self-test
```

CI runs `--check-shape` on `ubuntu-latest` and fails if the wedge shape regresses (indexed CTE at
100k ≈ full scan, or `AS MATERIALIZED` no longer ≫ inline). Absolute nanoseconds are not gated:
shared runners are too noisy. The CTE JSON artifact is uploaded for the shape gate. A **full report**
JSON (all illustrative benches, median of 5) is produced by the `benchmark report` CI job — not on
ordinary push/PR. Do not commit the raw JSON.

Refresh this table without a local run:

```sh
# once the job exists on the default branch:
gh workflow run ci.yml
# or push a commit whose message contains [benchmark-report]
gh run download <run-id> --name benchmark-report-json
python3 scripts/check_benchmark_shape.py --markdown-table benchmark-report.json
```

Paste the printed summary into the section below. Label the runner honestly (`ubuntu-latest` vs a
quiet local host). Concurrent worker counts follow `hardware_concurrency` (4 on GHA, 12 on a
12-thread laptop).

Equivalent flags if you invoke the binary directly:

```sh
cmake -S . -B build-benchmark \
  -DVERTEXDB_BUILD_TESTS=OFF \
  -DVERTEXDB_BUILD_BENCHMARKS=ON \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-benchmark
./build-benchmark/VertexDB_benchmarks \
  --benchmark_filter='BM_IndexedPointLookup|BM_FilteredSelect|BM_NonIndexedFilteredSelect|BM_ConcurrentPointLookups|BM_VectorRowStore|BM_PageRowStore|BM_BTreeRangeQuery|BM_Transaction|BM_Cte' \
  --benchmark_repetitions=5 \
  --benchmark_report_aggregates_only=true \
  --benchmark_min_time=0.5s \
  --benchmark_enable_random_interleaving=true
```

## Reporting

Benchmark output should be checked into documentation only as summarized tables or generated graphs, not raw build artifacts. Prefer **ratios from one run** over comparing absolute times across machines or days.

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

CI shape gates (median CPU time, same process):

| Ratio | Gate | Desired story |
| --- | --- | --- |
| indexed CTE 100k / indexed CTE 1k | ≤ 8× | win path stays roughly flat as N grows |
| scan CTE 100k / indexed CTE 100k | ≥ 20× | win at 100k is not a full scan |
| materialized CTE 1k / indexed CTE 1k | ≥ 50× | `AS MATERIALIZED` is far slower than inline |
| scan CTE 100k / scan CTE 1k | ≥ 10× | non-indexed baseline actually grows with N |

Local Release ratios are typically ~1×, ~10³–10⁴×, ~10⁴×, and ~10²×. The gates are conservative so
GHA noise does not fail a healthy run.

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

## Summary — 2026-08-10

Illustrative snapshot from GitHub Actions `ubuntu-latest` (not the CI shape gate). Google Benchmark
context: 4 logical CPUs, 3244 MHz, `release` build, host `runnervmvrwv9` (run
[31396669419](https://github.com/theCityCR/VertexDB/actions/runs/31396669419), artifact
`benchmark-report-json`). Median of 5 repetitions at `--benchmark_min_time=0.5s` with random
interleaving — the same settings as `scripts/run-benchmarks.sh`. Times are Google Benchmark **CPU
time**. Executor `Update`/`Delete` benches are omitted from this table (multi‑minute iterations under
page-image WAL); they remain in the binary for local runs. Concurrent workers stop at 4 because
`hardware_concurrency` on the runner is 4.

The shape gate still uses only ratios from the CTE subset; absolute nanoseconds here are illustrative
and will differ across runners and days.

| Benchmark | Arg | CPU time |
| --- | ---: | ---: |
| `BM_IndexedPointLookup` | 1,000 | ~50.1 ns |
| `BM_IndexedPointLookup` | 100,000 | ~50.2 ns |
| `BM_FilteredSelect` | 1,000 | ~1.16 µs |
| `BM_FilteredSelect` | 100,000 | ~1.17 µs |
| `BM_NonIndexedFilteredSelect` | 1,000 | ~106 µs |
| `BM_NonIndexedFilteredSelect` | 100,000 | ~19.8 ms |
| `BM_ConcurrentPointLookups` | 1 | ~25.5 µs |
| `BM_ConcurrentPointLookups` | 2 | ~48.1 µs |
| `BM_ConcurrentPointLookups` | 4 | ~154 µs |
| `BM_CteIndexedWinSelect` | 1,000 | ~1.96 µs |
| `BM_CteIndexedWinSelect` | 100,000 | ~1.97 µs |
| `BM_CteNonIndexedSelect` | 1,000 | ~152 µs |
| `BM_CteNonIndexedSelect` | 100,000 | ~25.7 ms |
| `BM_CteMaterializedSelect` | 1,000 | ~36.1 ms |
| `BM_CteMaterializedSelect` | 10,000 | ~428 ms |
| `BM_VectorRowStoreInsert` | 1,000 | ~63.2 µs |
| `BM_VectorRowStoreInsert` | 100,000 | ~6.12 ms |
| `BM_PageRowStoreInsert` | 1,000 | ~11.9 ms |
| `BM_PageRowStoreInsert` | 10,000 | ~150 ms |
| `BM_VectorRowStoreSelect` | 1,000 | ~1.56 ns |
| `BM_VectorRowStoreSelect` | 100,000 | ~1.56 ns |
| `BM_PageRowStoreSelect` | 1,000 | ~7.32 ns |
| `BM_PageRowStoreSelect` | 100,000 | ~7.38 ns |
| `BM_BTreeRangeQuery` | 1,000 | ~1.84 µs |
| `BM_BTreeRangeQuery` | 100,000 | ~442 µs |
| `BM_TransactionSnapshotRead` | 1,000 | ~1.16 µs |
| `BM_TransactionSnapshotRead` | 10,000 | ~1.17 µs |
| `BM_TransactionRollback` | 100 | ~144 ms |
| `BM_TransactionRollback` | 1,000 | ~1.35 s |

Takeaways from this run:

- Inlined CTE index-win stays flat as N grows (~2 µs); the non-indexed CTE baseline grows with table
  size (~26 ms at 100k). `AS MATERIALIZED` pays a large temp-build cost each iteration (~10⁴× slower
  than the index-win inline path here).
- Indexed filtered `SELECT` stays ~1.2 µs across 1k–100k rows; non-indexed filtered `SELECT` grows
  with N (~20 ms at 100k).
- Concurrent point lookups scale sub-linearly in CPU time as worker count rises (1 → 4 on this
  runner).
- Vector row-store appends remain much cheaper than page-store appends; mid-row `get` is ~5× faster
  on the vector store (contiguous vs page deserialize/cache).
- B+ range cost grows with result cardinality (half the table).
- Snapshot reads stay cheap; SQL `INSERT`+`ROLLBACK` undo is expensive relative to direct table
  inserts (executor/WAL buffering path).

## Remaining Follow-ups

- Refresh this illustrative absolute-time table from a new `benchmark-report-json` artifact (or a
  quiet local `scripts/run-benchmarks.sh`) after planner or storage changes. The shape gate already
  runs on every push/PR.
- Optional: Debug vs Release and sanitizer comparison tables.
- Optional: include executor `Update`/`Delete` once those paths are cheap enough for short
  `--benchmark_min_time` report runs.
