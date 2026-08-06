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
- `WITH` CTE inlining, derived tables, `WHERE col IN (SELECT …)`, cheapest-indexable `AND`
  extraction with residual filters, and `EXPLAIN`
- Typed table storage with schema validation, nullable columns, page-backed row storage (page-byte
  directory as source of truth with an LRU buffer-pool access cache), and stable row IDs via
  tombstones with free-list reuse (persisted across save/load)
- Maintained hash indexes and ordered B+ tree indexes with incremental leaf/internal split/merge
- Versioned binary persistence (page-payload + index-pages v4 `.tcrdb` snapshots; page-payload v3,
  sparse v2, and dense v1 still loadable), page-image WAL redo for DML, save checkpoints, and
  startup recovery
- Transaction state tracking, MVCC row-version storage with commit-aware snapshot isolation, undo-log
  rollback for DML, and transaction-atomic page-image WAL (DML deferred until `COMMIT`)
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
     +-- UndoLog
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
inlined), derived tables `FROM (SELECT …) [AS] alias`, `IN` subqueries, `EXPLAIN`, prepared
statements with positional parameters, table rename/drop/list operations, and `EXIT`.

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

- 114 GoogleTest cases covering parser, storage, indexes, execution, nested SQL rewrite/EXPLAIN,
  desired-behavior gaps, persistence (snapshot v4 index pages and page-image WAL), WAL recovery,
  concurrency, transactions, and regressions
- Coverage script enforces an 85% line coverage floor for the core library (latest local run:
  88.06%)
- Sanitizer script runs AddressSanitizer and UndefinedBehaviorSanitizer on supported platforms
- Benchmarks cover inserts, indexed and non-indexed filtered selects, CTE index-win vs full-scan
  baselines, update/delete throughput, and concurrent indexed point lookups

## Current Limitations

- Transactions use undo-log rollback for DML, commit-aware MVCC snapshot isolation for reads, and
  transaction-atomic page-image WAL (DML deferred until `COMMIT` as one batch, dropped on `ROLLBACK`)
- WAL DML redo uses page images (`PageImageRedo`); DDL remains logical SQL. Legacy `PhysicalRedo`
  row after-images remain replayable. Trailing torn WAL records are ignored so recovery replays the
  durable prefix
- Schema changes, `CREATE INDEX`, and `SAVE`/`LOAD` are rejected while a transaction is active
- Planner costs use live row counts and index distinct-key counts (no histograms); multi-index
  intersection and top-level `OR` index unions are not implemented
- Nested SQL is intentionally limited: no nested `WITH`, correlation, outer `JOIN` against a
  CTE/derived alias, `JOIN` inside `IN` subqueries, or expression/regex indexes
- SQL support is intentionally focused: no aggregates, grouping, or general DDL; joins are single
  equi-joins only

## Roadmap

1. Persist index pages with snapshots and evolve WAL redo toward page images — **done** (snapshot v4
   + `PageImageRedo`)
2. Add correlated subqueries, expression indexes, and `WITH … AS MATERIALIZED`
3. Extend SQL with aggregates, `COUNT`, `GROUP BY`, and multiple joins / richer join strategies
4. Add histograms / `ANALYZE` and multi-index AND optimization

Parallel product wedge (first milestone shipped): [CTE index wedge plan](docs/cte_index_wedge.md)
and [materialize vs inline comparison](docs/cte_materialize_comparison.md).

## Documentation

- [Architecture](docs/architecture.md)
- [Design status](docs/design.md)
- [SQL reference](docs/sql.md)
- [Testing](docs/testing.md)
- [Benchmarks](docs/benchmarks.md)
- [Deep features](docs/deep_features.md)
- [CTE index wedge plan](docs/cte_index_wedge.md)
- [CTE materialize vs inline comparison](docs/cte_materialize_comparison.md)

## License

MIT — see [LICENSE](LICENSE).
