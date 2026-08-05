# VertexDB

[![CI](https://github.com/theCityCR/VertexDB/actions/workflows/ci.yml/badge.svg)](https://github.com/theCityCR/VertexDB/actions/workflows/ci.yml)

VertexDB is a C++20 in-memory relational database engine. It implements a focused SQL execution
pipeline with typed storage, indexes, persistence, write-ahead logging, concurrency control,
transaction state, and MVCC read paths. A primary focus is index-aware query compilation for nested
SQL: CTE inlining, `IN` subqueries, sargable predicate extraction, and `EXPLAIN`.

The engine is intentionally small and educational: the goal is clear architecture, modern C++
design, correctness tests, and explicit tradeoffs in database internals—not production readiness.

## Features

- SQL tokenization, parsing, AST construction, query planning, and execution for a focused SQL subset
- `WITH` CTE inlining, `WHERE col IN (SELECT …)`, sargable `AND` extraction with residual filters,
  and `EXPLAIN`
- Typed table storage with schema validation, nullable columns, page-backed row storage, an LRU
  buffer pool, and stable row IDs via tombstones with free-list reuse (persisted across save/load)
- Maintained hash indexes and ordered B+ tree-style range lookup APIs
- Versioned binary persistence, logical WAL records, save checkpoints, and startup recovery
- Transaction state tracking, MVCC row-version storage, and snapshot rollback semantics
- GoogleTest suite, Google Benchmark targets, sanitizer/coverage scripts, and multi-platform CI (GCC, Clang, macOS Clang, MSVC)

## Architecture

```text
CLI
 |
 SQL Parser
 |
 Query Executor
 |
 +-- Query Planner
 +-- Storage Engine
 |   +-- Database / Table
 |   +-- RowStore
 |   |   +-- PageRowStore
 |   |   +-- VectorRowStore
 |   +-- BufferPool
 |
 +-- Index Manager
 |   +-- HashIndex
 |   +-- BTreeIndex
 |
 +-- Persistence
 |   +-- StorageManager
 |   +-- WriteAheadLog
 |
 +-- Concurrency / Transactions
     +-- LockManager
     +-- TransactionManager
     +-- MVCCRowStore
```

## SQL Surface

```sql
CREATE DATABASE company;
CREATE TABLE Employees (id INT, name STRING, salary DOUBLE);
CREATE INDEX idx_salary ON Employees(salary);
INSERT INTO Employees VALUES (1, "Alice", 120000.0), (2, "Bob", 90000.0);
SELECT name FROM Employees WHERE salary > 100000.0 ORDER BY salary DESC LIMIT 10;
UPDATE Employees SET salary = 150000.0 WHERE id = 1;
DELETE FROM Employees WHERE id = 2;
SAVE DATABASE;
LOAD DATABASE company;
BEGIN;
COMMIT;
ROLLBACK;
```

Also supported: nullable columns, compound predicates, single equi-joins, `WITH` CTEs (always
inlined), `IN` subqueries, `EXPLAIN`, prepared statements with positional parameters, table
rename/drop/list operations, and `EXIT`.

See [examples/](examples/) for a runnable walkthrough.

## Build And Test

```sh
cmake -S . -B build -DVERTEXDB_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Additional verification:

```sh
scripts/run-sanitizers.sh
scripts/run-coverage.sh
cmake -S . -B build-benchmark -DVERTEXDB_BUILD_TESTS=OFF -DVERTEXDB_BUILD_BENCHMARKS=ON
cmake --build build-benchmark
```

Run the CLI:

```sh
./build/VertexDB_cli
```

Or feed an example script:

```sh
./build/VertexDB_cli < examples/company.sql
```

## Testing And Quality

- 80 GoogleTest cases covering parser, storage, indexes, execution, nested SQL rewrite/EXPLAIN,
  desired-behavior gaps, persistence, WAL recovery, concurrency, transactions, and regressions
  (roadmap items use explicit `GTEST_SKIP` with reasons rather than locking incomplete behavior)
- Coverage script enforces an 85% line coverage floor for the core library
- Sanitizer script runs AddressSanitizer and UndefinedBehaviorSanitizer on supported platforms
- Benchmarks cover inserts, indexed and non-indexed filtered selects, update/delete throughput, and
  concurrent indexed point lookups

## Current Limitations

- `PageRowStore` mirrors typed rows into buffer-pool pages, but typed row storage is still the
  operational source of truth
- B+ tree writes rebuild a deterministic shallow layout instead of performing incremental
  split/merge operations
- Transactions expose MVCC read boundaries, but rollback still restores a database snapshot copy
- WAL recovery replays logical SQL operations rather than physical page redo records
- The planner is rule-based and does not yet collect table/index statistics for costing
- Nested SQL is intentionally limited: no derived tables, correlation, `JOIN` inside CTEs/`IN`
  subqueries, or expression/regex indexes
- SQL support is intentionally focused: no aggregates, grouping, or general DDL

## Roadmap

1. Make page storage the source of truth by deserializing rows from buffer-pool pages
2. Replace snapshot-copy rollback with undo records or commit-aware MVCC visibility
3. Implement incremental B+ tree split/merge with structural invariant tests
4. Add physical WAL redo/recovery tests, including simulated partial writes
5. Add table/index statistics and cost-based planning
6. Extend nested SQL (derived tables, correlation) and expression indexes
7. Extend SQL with aggregates, `COUNT`, `GROUP BY`, and broader join support

Parallel product wedge (engine behavior already present; packaging evidence): see
[CTE index wedge plan](docs/cte_index_wedge.md).

## Documentation

- [Architecture](docs/architecture.md)
- [Design status](docs/design.md)
- [SQL reference](docs/sql.md)
- [Testing](docs/testing.md)
- [Benchmarks](docs/benchmarks.md)
- [Deep features](docs/deep_features.md)
- [CTE index wedge plan](docs/cte_index_wedge.md)

## License

MIT — see [LICENSE](LICENSE).
