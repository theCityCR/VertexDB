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

[`cte_index_win.sql`](cte_index_win.sql) loads ~100 high-salary employees plus a few low-salary
rows, indexes `id`, then runs the wedge query:

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
