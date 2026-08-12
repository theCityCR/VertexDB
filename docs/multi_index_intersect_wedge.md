# Multi-Index Intersect Wedge Plan

## Goal

Ship one credible scenario where VertexDB chooses a **multi-index AND intersect** when that is
cheaper than probing a single equality index and residual-filtering the other conjunct.

This is a **demo wedge**, not a claim of general planner superiority. The engine behavior already
exists; this plan packages evidence so the story is repeatable and honest. It complements the
[CTE index wedge](cte_index_wedge.md) (rewrite / inlining) with a pure access-path composition story.

## Win query

```sql
CREATE INDEX idx_dept ON Employees(dept);
CREATE INDEX idx_city ON Employees(city);

SELECT name FROM Employees WHERE dept = 1 AND city = 1;
```

Desired plan shape:

- access path: `multi-index intersect on dept, city`
- no full table scan

Against a single-index residual plan (only `dept` indexed), the competing story is: probe ~N/D
candidates for `dept = 1`, then filter each by `city = 1`. Intersect probes both posting lists and
keeps only the intersection.

## Already done

Do not rebuild these:

1. `tryPlanAndIntersect` picks ≥2 equality (or expression-equality) index probes when estimated
   intersection cost beats the best single-index path (`src/planner/query_planner_access.cpp`).
2. Executor intersects sorted `RowId` lists (`src/execution/select_engine_scan.cpp`).
3. `EXPLAIN` surfaces `multi-index intersect on …`.
4. Focused tests in `tests/planner_intersect_union_tests.cpp`:
   - `MultiIndexIntersectChosenWhenCheaperThanSingleIndexResidual`
   - `MultiIndexIntersectReturnsOnlyRowsMatchingAllProbes`
   - `MultiIndexIntersectIncludesExpressionEquality`
   - `StatsDrivenPlannerPrefersSelectiveEqualityOverLowCardinality` (intersect *not* chosen when
     one probe is near-unique)
5. Semantics documented in `docs/sql.md`, `docs/design.md`, and `docs/deep_features.md`.

## What to build

Ordered by leverage. Engine work is not the bottleneck.

### 1. Demo SQL — done

Shipped as [`examples/multi_index_intersect_win.sql`](../examples/multi_index_intersect_win.sql)
(linked from `examples/README.md`):

- create table + indexes on `dept` and `city`
- 200 rows with medium-cardinality keys (`id % 10`, `(id / 10) % 10`)
- `EXPLAIN` / `SELECT` for the win query
- contrast table with only `dept` indexed (hash equality + residual `city`)

### 2. Scaled regression test — done

`PlannerBehaviorTests::ScaledMultiIndexIntersectUsesBothIndexes` seeds 10k employees via
`Table::insert`, then asserts `EXPLAIN` still chooses multi-index intersect (not a full scan) and
that `SELECT` returns only rows matching both predicates.

### 3. Microbenchmark — done

`BM_MultiIndexIntersectSelect` and `BM_SingleIndexResidualSelect` in
`benchmarks/storage_benchmarks.cpp` run the win query at 1k and 100k rows (both indexes vs only
`dept`). Expected shape is documented in `docs/benchmarks.md`.

### 4. BitmapAnd parity comparison — done

External comparison (preferred narrative path):

- Script: [`scripts/compare_bitmap_and.sh`](../scripts/compare_bitmap_and.sh) — VertexDB `EXPLAIN`
  always; Postgres `EXPLAIN` when Docker or local `psql` is available.
- Note: [`bitmap_and_comparison.md`](bitmap_and_comparison.md) — captured VertexDB plan, illustrative
  Postgres BitmapAnd / Bitmap Heap Scan shapes, and honest limitations.

### 5. Wedge write-up — done

This living plan plus a short pointer from `docs/deep_features.md`. The longer Postgres side-by-side
lives in [bitmap_and_comparison.md](bitmap_and_comparison.md).

## Demo

### Query

```sql
CREATE INDEX idx_dept ON Employees(dept);
CREATE INDEX idx_city ON Employees(city);

SELECT name FROM Employees WHERE dept = 1 AND city = 1;
```

Run it:

```sh
./build/VertexDB_cli < examples/multi_index_intersect_win.sql
```

### Sample `EXPLAIN`

```text
multi-index intersect on dept, city
```

VertexDB probes both equality indexes and intersects sorted `RowId` lists when that estimate beats
a single index plus residual.

### Why single-index residual loses at scale

With medium-cardinality keys, each equality probe returns a large posting list (~N/10). Filtering
the second predicate as a residual visits every candidate from the first probe. Intersecting two
lists keeps work proportional to the lists and the (much smaller) intersection.

Postgres expresses the same idea with `BitmapAnd` + Bitmap Heap Scan. VertexDB’s educational
`EXPLAIN` names the intersected columns directly. Details:
[bitmap_and_comparison.md](bitmap_and_comparison.md).

### Limitations (honest)

- Intersect is **cost-gated**: a near-unique equality (e.g. `id = 1 AND dept = 1` with `id` indexed)
  prefers HashEq + residual, not intersect.
- Fully indexable nested `OR` under `AND` uses composite Intersect∪Union; a nested `OR` with any
  non-equality-indexable arm stays an AND residual (no partial nested-OR complementary scan yet).
- This is one deliberate query-class win, not a claim that VertexDB beats Postgres in general.

### Evidence checklist

| Artifact | Role |
|----------|------|
| [`examples/multi_index_intersect_win.sql`](../examples/multi_index_intersect_win.sql) | Runnable demo |
| `PlannerBehaviorTests::ScaledMultiIndexIntersectUsesBothIndexes` | Scaled plan regression |
| `BM_MultiIndexIntersectSelect` / `BM_SingleIndexResidualSelect` | Cost shape at 1k/100k |
| [`scripts/run-benchmarks.sh --check-shape`](../scripts/run-benchmarks.sh) | CI gate: residual ≫ intersect; residual grows; intersect growth bounded |
| [`scripts/compare_bitmap_and.sh`](../scripts/compare_bitmap_and.sh) | Live Postgres/VertexDB plans |

## First milestone — shipped

Items **1–5** are done. The one-liner:

> For this query class, VertexDB intersects two equality indexes when that is cheaper than one index plus a residual; a single-index residual plan would touch far more candidates.

## Out of scope for this wedge

- Top-level / partial `OR` union (separate access-path story; not part of this AND-intersect demo).
- Join algorithm selection (hash vs index nested-loop).
- Winning only because Postgres sometimes skips BitmapAnd under odd stats — that is a heuristic
  quirk, not a product story.

## Definition of done

- [x] Demo script runs through `VertexDB_cli` and shows the multi-index intersect plan.
- [x] Scaled test fails if the planner regresses to a full scan (or drops intersect) for the win data.
- [x] Benchmark exercises intersect vs single-index residual at large N and is listed in
  `docs/benchmarks.md`.
- [x] CI fails if intersect cost shape regresses (`scripts/run-benchmarks.sh --check-shape`).
- [x] Docs state the wedge, the evidence, and the limitations without overclaiming.
