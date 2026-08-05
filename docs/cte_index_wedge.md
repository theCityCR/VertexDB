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

### 2. Scaled regression test

Add a desired-behavior test at ~10k–100k rows that asserts:

- `EXPLAIN` still chooses hash index lookup + inlined CTE + residual
- the query returns the correct single-row result

Today’s CTE+index tests use tiny tables; scale is what makes “we did not accidentally scan”
credible.

### 3. Microbenchmark

Extend `benchmarks/storage_benchmarks.cpp` with the CTE win query at 100k rows, plus a
non-indexed / full-scan baseline. Document the expected shape in `docs/benchmarks.md`.

### 4. Materialize-vs-inline comparison

Pick one (prefer external for narrative; add internal if CI needs a stable A/B):

- **External:** script that runs the same query shape in Postgres with
  `WITH … AS MATERIALIZED` vs VertexDB; capture `EXPLAIN` / timings into a short comparison note
  under `docs/`.
- **Internal:** test-only fence path that materializes the CTE then filters (not user-facing
  `AS MATERIALIZED` SQL) for A/B timing that cannot drift with Postgres versions.

### 5. Wedge write-up

Keep this document as the living plan. When the milestone ships, add a short “Demo” section here
(or in `docs/deep_features.md`) with:

- the query
- sample VertexDB `EXPLAIN` output
- why materializing CTEs lose the `id` index
- honest limitations (single-table CTE, heuristic costs, no claim of beating Postgres in general)

## First milestone

Ship items **1–3** and a minimal comparison note from **4** or **5**. That is enough to say:

> For this query class, VertexDB uses the base-table index; a materializing CTE does not.

## Out of scope for this wedge

- Full cost-based optimization and table statistics (separate roadmap item).
- User-facing `WITH … AS MATERIALIZED` grammar (optional later; not required to demo the win).
- Multi-index AND, `OR` index union, expression indexes, or join access-path planning.
- Winning only because VertexDB always picks hash lookup when Postgres sometimes does not—that is
  a heuristic quirk, not a product story.

## Definition of done

- Demo script runs through `VertexDB_cli` and shows the index + inlined CTE plan.
- Scaled test fails if the planner regresses to a full scan for the win query.
- Benchmark exercises the CTE path at large N and is listed in `docs/benchmarks.md`.
- Docs state the wedge, the evidence, and the limitations without overclaiming.
