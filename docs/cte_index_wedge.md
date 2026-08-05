# CTE Index Wedge Plan

## Goal

Ship one credible scenario where VertexDB makes a better access-path choice than engines that
materialize CTEs by default: outer filters on an inlined `WITH` hit a base-table index instead of
scanning a temporary CTE result.

This is a **demo wedge**, not a claim of general planner superiority. The engine behavior already
exists; this plan packages evidence so the story is repeatable and honest.

## Win query

```sql
CREATE INDEX idx_id ON Employees(id);

WITH high AS (
  SELECT id, name, salary FROM Employees WHERE salary > 100000.0
)
SELECT name FROM high WHERE id = 1;
```

Desired plan shape:

- rewrite note: `inlined CTE high`
- access path: hash index equality lookup on `id`
- residual: body filter `salary > 100000.0`

Against a materializing CTE, the competing story is: build all high-salary rows, then filter by
`id`. VertexDB probes `id` first and applies the salary predicate as a residual.

## Already done

Do not rebuild these:

1. `rewriteSelect` always inlines CTEs and AND-merges outer predicates into the body.
2. Rule-based planner picks one sargable conjunct; remaining conjuncts stay residual.
3. `EXPLAIN` surfaces access path, residual status, and rewrite notes.
4. Focused tests:
   - `NestedSqlTests::CteInliningUsesBaseTableIndex`
   - `DesiredBehaviorTests::CteInliningLeavesBodyFilterAsResidualWhenOuterUsesIndex`
   - rewriter / residual / `IN (SELECT …)` coverage in `tests/nested_sql_tests.cpp` and
     `tests/desired_behavior_tests.cpp`
5. Semantics documented in `docs/sql.md`, `docs/design.md`, and `docs/deep_features.md`.

## What to build

Ordered by leverage. Engine work is not the bottleneck.

### 1. Demo SQL — done

Shipped as [`examples/cte_index_win.sql`](../examples/cte_index_win.sql) (linked from
`examples/README.md`):

- create table + index on `id`
- 101 high-salary rows (including target `id = 1`) plus 10 low-salary rows
- `EXPLAIN` / `SELECT` for the win query and the equivalent flat `WHERE`

Optional follow-up: add the same `EXPLAIN` block to `examples/company.sql`.

### 2. Scaled regression test — done

`DesiredBehaviorTests::ScaledCteWinQueryUsesHashIndexAndResidual` seeds 10k employees (mostly
high-salary) via `Table::insert`, then asserts `EXPLAIN` still chooses hash index lookup + inlined
CTE + residual (not a full scan) and that the win query returns Alice through the executor.

`createIndex` at this scale relies on deferred B+ tree layout rebuild (layout rebuilt on read, not
on every key insert). The CTE microbenchmark can push toward 100k rows.

### 3. Microbenchmark — done

`BM_CteIndexedWinSelect` and `BM_CteNonIndexedSelect` in `benchmarks/storage_benchmarks.cpp` run the
win query at 1k and 100k rows (indexed hash path vs full-scan baseline). Expected shape is documented
in `docs/benchmarks.md`.

### 4. Materialize-vs-inline comparison — done

External comparison (preferred narrative path):

- Script: [`scripts/compare_cte_materialize.sh`](../scripts/compare_cte_materialize.sh) — VertexDB
  `EXPLAIN` always; Postgres `AS MATERIALIZED` / `AS NOT MATERIALIZED` when Docker or local `psql`
  is available.
- Note: [`docs/cte_materialize_comparison.md`](cte_materialize_comparison.md) — captured VertexDB
  plan, illustrative Postgres plan shapes, microbenchmark cost proxy, and honest limitations.

Skipped for now: internal test-only materialize fence (CI A/B). The indexed vs non-indexed CTE
microbenchmarks already provide an in-repo cost baseline.

### 5. Wedge write-up — done

Demo section below (this living plan) plus a short pointer from `docs/deep_features.md`. The longer
Postgres side-by-side lives in [cte_materialize_comparison.md](cte_materialize_comparison.md).

## Demo

### Query

```sql
CREATE INDEX idx_id ON Employees(id);

WITH high AS (
  SELECT id, name, salary FROM Employees WHERE salary > 100000.0
)
SELECT name FROM high WHERE id = 1;
```

Run it:

```sh
./build/VertexDB_cli < examples/cte_index_win.sql
```

### Sample `EXPLAIN`

```text
hash index equality lookup on id
residual filter applied after index lookup
inlined CTE high
residual: yes
```

VertexDB always inlines the CTE, AND-merges the outer `id = 1` into the body, probes the `id` hash
index, and applies `salary > 100000.0` as a residual filter.

### Why materializing CTEs lose the `id` index

If the CTE is materialized first, the engine builds a temporary result of all high-salary rows, then
filters that temp by `id = 1`. The base-table index on `Employees(id)` is no longer in play for the
outer predicate — you pay for the large intermediate set even though only one row is needed.

Postgres can show that fence explicitly with `WITH … AS MATERIALIZED` (CTE Scan over a scanned CTE
body). The same engine can also inline with `AS NOT MATERIALIZED`. VertexDB’s default is the
inline path so nested SQL does not silently drop indexes. Details:
[cte_materialize_comparison.md](cte_materialize_comparison.md).

### Limitations (honest)

- Single-table CTEs only (no `JOIN` / nested `WITH` inside CTE bodies).
- Rule-based access paths and heuristic costs — not a statistics-driven optimizer.
- This is one deliberate query-class win, not a claim that VertexDB beats Postgres in general.
- `UPDATE` / `DELETE` and joins still bypass this index access-path planner.

### Evidence checklist

| Artifact | Role |
|----------|------|
| [`examples/cte_index_win.sql`](../examples/cte_index_win.sql) | Runnable demo |
| `DesiredBehaviorTests::ScaledCteWinQueryUsesHashIndexAndResidual` | Scaled plan regression |
| `BM_CteIndexedWinSelect` / `BM_CteNonIndexedSelect` | Cost shape at 1k/100k |
| [`scripts/compare_cte_materialize.sh`](../scripts/compare_cte_materialize.sh) | Live Postgres/VertexDB plans |

## First milestone — shipped

Items **1–5** are done. The one-liner:

> For this query class, VertexDB uses the base-table index; a materializing CTE does not.

## Out of scope for this wedge

- Full cost-based optimization and table statistics (separate roadmap item).
- User-facing `WITH … AS MATERIALIZED` grammar (optional later; not required to demo the win).
- Multi-index AND, `OR` index union, expression indexes, or join access-path planning.
- Winning only because VertexDB always picks hash lookup when Postgres sometimes does not—that is
  a heuristic quirk, not a product story.

## Definition of done

- [x] Demo script runs through `VertexDB_cli` and shows the index + inlined CTE plan.
- [x] Scaled test fails if the planner regresses to a full scan for the win query.
- [x] Benchmark exercises the CTE path at large N and is listed in `docs/benchmarks.md`.
- [x] Docs state the wedge, the evidence, and the limitations without overclaiming.
