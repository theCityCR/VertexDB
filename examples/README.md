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

Each statement should end with `;`. Type `EXIT;` to quit.
