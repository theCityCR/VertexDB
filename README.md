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

See [AGENTS.md](AGENTS.md) for a short agent-oriented layout map and
[docs/architecture.md](docs/architecture.md) for the module map and data flow. High level:

```text
CLI → Parser → QueryExecutor → Planner / Storage / Indexes / Persistence / Txn
```

Focused executor TUs handle SELECT, subquery/CTE, and WAL recovery; `Table` and `.tcrdb` codecs are
likewise split by concern.

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

Also supported: nullable columns, compound predicates (`AND`/`OR`, `LIKE`, regex `~`), left-deep
`INNER` / `LEFT` / `RIGHT` / `FULL` join chains and `CROSS JOIN` with `ON col op col` (`=`, `<`, `>`; none for `CROSS`), aggregates
(`COUNT`/`SUM`/`AVG`/`MIN`/`MAX`) with `GROUP BY`, `WITH` CTEs (always inlined by default; nesting
depth up to 3), derived tables `FROM (SELECT …) [AS] alias`, `IN`/`EXISTS` subqueries, `EXPLAIN`,
prepared statements that store a typed AST with `?` parameter slots, table rename/drop/list
operations, and `EXIT`.

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
python3 scripts/check_benchmark_shape.py --self-test
scripts/run-benchmarks.sh --check-shape
# optional full report (slow); or trigger the CI `benchmark report` job instead
# scripts/run-benchmarks.sh
# python3 scripts/check_benchmark_shape.py --markdown-table build-benchmark/benchmark-report.json
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

- 183 GoogleTest cases across parser, storage, indexes, execution, nested SQL, planner behavior,
  transactions, persistence/WAL, aggregates/prepared statements, deep features, and regressions
  (see [docs/testing.md](docs/testing.md) for file ownership)
- Coverage script enforces an 85% line coverage floor for the core library (latest local run:
  85.19%)
- Sanitizer script runs AddressSanitizer and UndefinedBehaviorSanitizer on supported platforms
- Benchmarks cover inserts, indexed and non-indexed filtered selects, CTE index-win vs full-scan
  and MATERIALIZED baselines, page vs vector row-store, B+ range, transaction snapshot/rollback,
  update/delete throughput, and concurrent indexed point lookups; summarized in
  [docs/benchmarks.md](docs/benchmarks.md). CI gates CTE **cost shape** (ratios, not absolute ns)
  via `scripts/run-benchmarks.sh --check-shape`. A full absolute-time report for doc refresh is a
  separate `benchmark report` CI job, not every push/PR.

## Current Limitations

- Transactions use undo-log rollback for DML, commit-aware MVCC snapshot isolation for reads, and
  transaction-atomic page-image WAL (DML deferred until `COMMIT` as one batch, dropped on `ROLLBACK`)
- WAL DML redo uses page images (`PageImageRedo`); DDL remains logical SQL. Legacy `PhysicalRedo`
  row after-images remain replayable. Trailing torn WAL records are ignored so recovery replays the
  durable prefix
- Schema changes, `CREATE INDEX`, and `SAVE`/`LOAD` are rejected while a transaction is active
- Planner costs use live row counts, index distinct-key counts, and optional `ANALYZE` histograms
  for range/`IN` selectivity; multi-index AND intersection and top-level `OR` union (including
  partial union of indexable arms with a residual OR complementary scan) are supported
- Nested SQL is intentionally limited: `WITH` nesting deeper than depth 3, correlation deeper than
  four outer frames, or `WITH RECURSIVE`. Outer `JOIN` against a CTE/derived alias force-materializes the CTE. `JOIN` inside `IN`/`EXISTS` is
  supported. Expression indexes cover column / unary minus / `+/-` literal /
  `trigram(column)` (substring `LIKE`); prefix `LIKE` uses ordered indexes; regex `~` is residual
  full-scan. `FROM` / `JOIN` table aliases and `WITH` / derived tables inside `IN`/`EXISTS` are
  supported. Parser failures report `line`/`column` via `ParseError`.
- Aggregates and `GROUP BY` are supported; non-aggregated selected columns must appear in `GROUP BY`.
  Joins are left-deep `INNER` / `LEFT` / `RIGHT` / `FULL` chains and `CROSS JOIN` with `ON` `=` /
  `<` / `>` (no `ON` for `CROSS`)

## Roadmap

Forward-looking work lives in [docs/design.md](docs/design.md) (Next Steps). Shipped milestones
include snapshot v4 + page-image WAL, correlated subqueries / expression indexes / materialized CTEs,
aggregates and multi-join, histograms / multi-index AND and top-level OR union (including partial OR),
`WITH` nesting depth up to 3 and correlation through four outer frames, `INNER`/`LEFT`/`RIGHT`/`FULL`/`CROSS` joins with
non-equi `ON`, `LIKE` / regex predicates (prefix and trigram index paths), join-table aliases, parse
diagnostics with source positions, CI CTE cost-shape gating, and a dated absolute-time benchmark
summary (last refreshed 2026-08-10 from the CI `benchmark report` artifact).

Parallel product wedge: [CTE index wedge plan](docs/cte_index_wedge.md) and
[materialize vs inline comparison](docs/cte_materialize_comparison.md).

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
