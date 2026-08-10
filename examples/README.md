# Examples

Sample SQL scripts you can feed into the VertexDB CLI.

## Prerequisites

Build the CLI first:

```sh
cmake -S . -B build -DVERTEXDB_BUILD_TESTS=ON
cmake --build build
```

## Company walkthrough

[`company.sql`](company.sql) creates a small company schema, inserts employees and departments,
runs filtered and join queries, exercises a rolled-back transaction, and saves the database.
For the full SQL surface (including `RIGHT`/`FULL`/`CROSS` joins, `JOIN` inside `IN`/`EXISTS`,
CTE join targets, and minimal `WITH RECURSIVE`), see [docs/sql.md](../docs/sql.md).

```sh
./build/VertexDB_cli < examples/company.sql
```

You can also paste statements interactively:

```sh
./build/VertexDB_cli
```

Each statement should end with `;` and must fit on a single line (the CLI reads one
statement per input line). Type `EXIT;` to quit.

## CTE index win

[`cte_index_win.sql`](cte_index_win.sql) loads 101 high-salary employees plus 10 low-salary rows,
indexes `id`, then runs the wedge query:

```sql
WITH high AS (
  SELECT id, name, salary FROM Employees WHERE salary > 100000.0
)
SELECT name FROM high WHERE id = 1;
```

`EXPLAIN` should report a hash index equality lookup on `id`, a residual salary filter, and
`inlined CTE high`. A materializing CTE would instead build the full high-salary set before
filtering by `id`. The script also shows the equivalent flat `WHERE` for comparison.

```sh
./build/VertexDB_cli < examples/cte_index_win.sql
```

See [docs/cte_index_wedge.md](../docs/cte_index_wedge.md) for the full wedge plan and
[docs/cte_materialize_comparison.md](../docs/cte_materialize_comparison.md) for the Postgres
materialize vs inline comparison. Regenerate live plans with:

```sh
scripts/compare_cte_materialize.sh
```

## Multi-index intersect win

[`multi_index_intersect_win.sql`](multi_index_intersect_win.sql) loads 200 employees with
medium-cardinality `dept` / `city` keys, indexes both columns, then runs:

```sql
SELECT name FROM Employees WHERE dept = 1 AND city = 1;
```

`EXPLAIN` should report `multi-index intersect on dept, city`. A contrast table with only `dept`
indexed shows hash equality on `dept` plus a residual `city` filter — the cost model intersects
only when both probes are available and cheaper than a single index + residual.

```sh
./build/VertexDB_cli < examples/multi_index_intersect_win.sql
```

See [docs/multi_index_intersect_wedge.md](../docs/multi_index_intersect_wedge.md) and the Postgres
BitmapAnd parity note in [docs/bitmap_and_comparison.md](../docs/bitmap_and_comparison.md).
Regenerate live plans with:

```sh
scripts/compare_bitmap_and.sh
```
