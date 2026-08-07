# CTE Materialize vs Inline Comparison

This note is the external comparison for the [CTE index wedge](cte_index_wedge.md): the same query
shape against VertexDB’s always-inline CTEs and Postgres `WITH … AS MATERIALIZED` /
`AS NOT MATERIALIZED`.

## Win query

```sql
WITH high AS (
  SELECT id, name, salary FROM Employees WHERE salary > 100000.0
)
SELECT name FROM high WHERE id = 1;
```

Intent: many rows satisfy `salary > 100000.0`, but only one row has `id = 1`. An engine that
**materializes** the CTE builds (or scans) the large high-salary set, then filters by `id`. An
engine that **inlines** can probe an `id` index first and apply salary as a residual.

## VertexDB (captured)

With `CREATE INDEX idx_id ON Employees(id)`, `EXPLAIN` reports:

```text
hash index equality lookup on id
residual filter applied after index lookup
inlined CTE high
residual: yes
```

The equivalent flat form `WHERE salary > 100000.0 AND id = 1` chooses the same hash lookup + residual
(without the inlining note). Runnable demo: [`examples/cte_index_win.sql`](../examples/cte_index_win.sql).

Cost shape at scale (debug smoke run of the microbenchmarks):

| Path | 1k rows | 100k rows |
|------|---------|-----------|
| Indexed inlined CTE (`BM_CteIndexedWinSelect`) | ~40 µs | ~27 µs |
| Full-scan CTE baseline (`BM_CteNonIndexedSelect`) | ~1.7 ms | ~181 ms |

Indexed cost stays near point-lookup; the non-indexed baseline grows with table size. That baseline
is the in-tree stand-in for “materialize then filter” cost when no external DB is available.

## Postgres (illustrative plans)

Postgres can fence a CTE with `AS MATERIALIZED` or allow inlining with `AS NOT MATERIALIZED`
(PostgreSQL 12+). On a table with a primary key / index on `id` and many high-salary rows, the plans
typically look like:

**`AS MATERIALIZED`** — CTE result is built, then filtered (index on `id` is not used for the outer
predicate against the base table):

```text
CTE Scan on high
  Filter: (id = 1)
  CTE high
    ->  Seq Scan on employees
          Filter: (salary > '100000'::double precision)
```

**`AS NOT MATERIALIZED`** — optimizer may collapse to a base-table index probe:

```text
Index Scan using employees_pkey on employees
  Index Cond: (id = 1)
  Filter: (salary > '100000'::double precision)
```

Exact node names and scan types vary with statistics and Postgres version; the structural point is
stable: **materialization fences the outer `id = 1` away from the base-table index**, while inlining
(or VertexDB’s always-inline rewrite) can use it.

These Postgres snippets are illustrative. Regenerate live `EXPLAIN` output with:

```sh
cmake -S . -B build && cmake --build build --target VertexDB_cli
scripts/compare_cte_materialize.sh
# optional: ROWS=100000 OUT=docs/cte_materialize_capture.md scripts/compare_cte_materialize.sh
```

The script always prints the VertexDB plan. The Postgres half runs when Docker’s daemon is up (image
`postgres:16` by default) or when local `psql` can connect via the usual `PG*` environment
variables.

## What this does *not* claim

- VertexDB is not “better than Postgres” in general. Postgres can already inline with
  `AS NOT MATERIALIZED` or by writing the flat `WHERE`.
- The win is a **deliberate default**: nested SQL should not silently lose base-table indexes.
- Limitations: cost-based planning with row/distinct-key stats (not histograms); nested `WITH`
  deeper than one level and correlation deeper than two outer frames remain unsupported.
  User-facing `AS MATERIALIZED` / `AS NOT MATERIALIZED` are available; default remains inline.

## Related

- Plan: [cte_index_wedge.md](cte_index_wedge.md)
- Demo: [examples/cte_index_win.sql](../examples/cte_index_win.sql)
- Benchmarks: [benchmarks.md](benchmarks.md) (`BM_CteIndexedWinSelect` /
  `BM_CteNonIndexedSelect`)
- Script: [scripts/compare_cte_materialize.sh](../scripts/compare_cte_materialize.sh)
