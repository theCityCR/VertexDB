# BitmapAnd Parity Comparison

This note is the external comparison for the
[multi-index intersect wedge](multi_index_intersect_wedge.md): the same `AND` of two equality
predicates against VertexDB’s multi-index intersect and Postgres Bitmap Index Scan /
`BitmapAnd` / Bitmap Heap Scan.

## Win query

```sql
SELECT name FROM Employees WHERE dept = 1 AND city = 1;
```

Intent: both `dept` and `city` are medium-cardinality and indexed. An engine that probes only one
index visits a large posting list and residual-filters the other predicate. An engine that
**intersects** two index probes keeps candidates that match both keys.

## VertexDB (captured)

With `CREATE INDEX` on both `dept` and `city`, `EXPLAIN` reports:

```text
multi-index intersect on dept, city
```

With only `dept` indexed, the same SQL chooses hash equality on `dept` and applies `city = 1` as a
residual filter. Runnable demo:
[`examples/multi_index_intersect_win.sql`](../examples/multi_index_intersect_win.sql).

Cost shape at scale is gated in CI (`BM_MultiIndexIntersectSelect` vs
`BM_SingleIndexResidualSelect`); see [benchmarks.md](benchmarks.md). Absolute nanoseconds land in
the illustrative summary after the next `[benchmark-report]` refresh.

## Postgres (illustrative plans)

On a table with btree indexes on `dept` and `city` and enough rows for bitmap plans to win, Postgres
typically looks like:

```text
Bitmap Heap Scan on employees
  Recheck Cond: ((dept = 1) AND (city = 1))
  ->  BitmapAnd
        ->  Bitmap Index Scan on employees_dept_idx
              Index Cond: (dept = 1)
        ->  Bitmap Index Scan on employees_city_idx
              Index Cond: (city = 1)
```

Exact node names vary with statistics and Postgres version; the structural point is stable:
**two index probes combined before (or as part of) heap access**, not a single index plus a
row-by-row residual of the other equality when both indexes are selective enough.

These Postgres snippets are illustrative. Regenerate live `EXPLAIN` output with:

```sh
cmake -S . -B build && cmake --build build --target VertexDB_cli
scripts/compare_bitmap_and.sh
# optional: ROWS=100000 OUT=docs/bitmap_and_capture.md scripts/compare_bitmap_and.sh
```

The script always prints the VertexDB plan. The Postgres half runs when Docker’s daemon is up (image
`postgres:16` by default) or when local `psql` can connect via the usual `PG*` environment
variables.

## What this does *not* claim

- VertexDB is not “better than Postgres” in general. Postgres BitmapAnd is the mature production
  analogue; this wedge is **parity + readable educational EXPLAIN**.
- The win is a **deliberate cost choice**: when two medium-cardinality equality indexes beat one
  index + residual, intersect.
- Limitations: near-unique equalities prefer HashEq + residual; nested `OR` under `AND` with no
  equality-indexable arm stays residual; partially or fully indexable nested OR uses composite
  Intersect∪Union (non-indexable arms as complementary residual under the outer AND).

## Related

- Plan: [multi_index_intersect_wedge.md](multi_index_intersect_wedge.md)
- Demo: [examples/multi_index_intersect_win.sql](../examples/multi_index_intersect_win.sql)
- Benchmarks: [benchmarks.md](benchmarks.md) (`BM_MultiIndexIntersectSelect` /
  `BM_SingleIndexResidualSelect`; CI shape gate: `scripts/run-benchmarks.sh --check-shape`)
- Sister wedge: [cte_index_wedge.md](cte_index_wedge.md)
