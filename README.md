# VertexDB

[![CI](https://github.com/theCityCR/VertexDB/actions/workflows/ci.yml/badge.svg)](https://github.com/theCityCR/VertexDB/actions/workflows/ci.yml)

VertexDB is a C++20 in-memory relational database engine. It implements a focused SQL execution
pipeline with typed storage, indexes, persistence, write-ahead logging, concurrency control,
transaction state, and MVCC read paths. A primary focus is index-aware query compilation for nested
SQL: CTE inlining, `IN` subqueries, sargable predicate extraction, `EXPLAIN`, and `EXPLAIN ANALYZE`.

The engine is intentionally small and educational: the goal is clear architecture, modern C++
design, correctness tests, and explicit tradeoffs in database internals—not production readiness.

## Features

- SQL tokenization, parsing, AST construction, query planning, and execution for a focused SQL subset
- `WITH` CTE inlining, derived tables, `WHERE col IN (SELECT …)`, cheapest-indexable `AND`
  extraction with residual filters, `EXPLAIN`, and `EXPLAIN ANALYZE` (actual vs estimated)
- Typed table storage with schema validation, nullable columns, page-backed row storage (page-byte
  directory as source of truth with an LRU buffer-pool access cache), and stable row IDs via
  tombstones with free-list reuse (persisted across save/load)
- Maintained hash indexes and ordered B+ tree indexes with incremental leaf/internal split/merge
- Versioned binary persistence (page-payload + index-pages + constraint flags + CHECK + FOREIGN KEY
  v9 `.tcrdb` snapshots; composite UNIQUE/PK v8, FK v7, CHECK v6, constraint flags v5, index-pages
  v4, page-payload v3, sparse v2, and dense v1 still loadable), page-image WAL redo for
  DML with flush+fsync on append (durable `COMMIT` / autocommit), save checkpoints, and startup
  recovery
- Single- and multi-column `PRIMARY KEY` / `UNIQUE` constraints with auto-maintained (composite)
  indexes and duplicate `INSERT`/`UPDATE` rejection; simple `CHECK` constraints (column comparisons with
  `AND`/`OR`); single- and multi-column `FOREIGN KEY` (`REFERENCES`, `NO ACTION` / `CASCADE` /
  `SET NULL`)
- Transaction state tracking, MVCC row-version storage with commit-aware snapshot isolation, undo-log
  rollback for DML and transactional catalog DDL (incl. `ALTER TABLE`), and transaction-atomic
  page-image WAL (DML deferred until `COMMIT`, then durable-synced)
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
DROP DATABASE company;
CREATE TABLE Employees (id INT, name STRING, salary DOUBLE);
CREATE TABLE Accounts (id INT PRIMARY KEY, email STRING UNIQUE);
CREATE TABLE Pay (id INT PRIMARY KEY, salary DOUBLE CHECK (salary > 0.0));
CREATE TABLE Orders (id INT PRIMARY KEY, customer_id INT REFERENCES Customers(id));
CREATE TABLE Enrollments (student_id INT, course_id INT, PRIMARY KEY (student_id, course_id));
ALTER TABLE Employees ADD COLUMN nickname STRING NULL;
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

Also supported: nullable columns and explicit `NOT NULL`, single- and multi-column `PRIMARY KEY` / `UNIQUE`,
simple `CHECK` (column comparisons with `AND`/`OR`), single- and multi-column `FOREIGN KEY`
(`NO ACTION` / `CASCADE` / `SET NULL`), `ALTER TABLE ADD COLUMN … NULL` / `DROP COLUMN` (dependency
rejection; transactional undo + logical WAL),
compound predicates
(`AND`/`OR`, `LIKE`, regex `~`), left-deep
`INNER` / `LEFT` / `RIGHT` / `FULL` join chains and `CROSS JOIN` with `ON col op col` (`=`, `<`, `>`; none for `CROSS`), aggregates
(`COUNT`/`SUM`/`AVG`/`MIN`/`MAX`) with `GROUP BY`, `WITH` CTEs (always inlined by default; nesting
depth up to 6; `WITH RECURSIVE` with `UNION` / `UNION ALL`, including multiple independent or mutually
recursive CTEs and optional `AS ACCUMULATOR` binding), derived tables `FROM (SELECT …) [AS] alias`,
`IN`/`EXISTS` subqueries, set operations
(`UNION` / `UNION ALL` / `INTERSECT` / `INTERSECT ALL` / `EXCEPT` / `EXCEPT ALL`), `EXPLAIN` /
`EXPLAIN ANALYZE`,
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

- 395 GoogleTest cases across parser, storage, indexes, execution, nested SQL (CTE / correlation /
  subquery / recursive), set operations (`UNION` / `UNION ALL` / `INTERSECT` / `INTERSECT ALL` /
  `EXCEPT` / `EXCEPT ALL`), planner behavior (access / intersect-union / explain / mutation /
  join-stats), transactions, persistence/WAL, aggregates/prepared statements, constraints
  (`PRIMARY KEY` / `UNIQUE` / `NOT NULL` / `CHECK` / `FOREIGN KEY`), deep features, and regressions (see [docs/testing.md](docs/testing.md)
  for file ownership)
- Coverage script enforces an 85% line coverage floor for the core library (latest local run:
  86.43%)
- Sanitizer script runs AddressSanitizer and UndefinedBehaviorSanitizer on supported platforms
- Benchmarks cover inserts, indexed and non-indexed filtered selects, CTE index-win vs full-scan
  and MATERIALIZED baselines, multi-index intersect vs single-index residual, page vs vector
  row-store, B+ range, transaction snapshot/rollback, update/delete throughput, and concurrent
  indexed point lookups; summarized in [docs/benchmarks.md](docs/benchmarks.md). CI gates wedge
  **cost shape** (CTE + intersect ratios, not absolute ns) via
  `scripts/run-benchmarks.sh --check-shape`. A full absolute-time report for doc refresh is a
  separate `benchmark report` CI job, not every push/PR.

## Current Limitations

- Transactions use undo-log rollback for DML and transactional catalog DDL (incl. `ALTER TABLE`),
  commit-aware MVCC snapshot isolation for reads, and transaction-atomic page-image WAL (DML
  deferred until `COMMIT` as one batch, dropped on `ROLLBACK`). SI prevents dirty reads and hides
  post-`BEGIN` commits; SSI aborts later committers
  on overlapping read/write sets (write skew / write–write) and on insert phantoms (predicate
  SIREAD vs insert/update images; OR of column leaves and column `LIKE` use real predicates;
  regex / subquery / expression-index probes use relation-membership fallbacks). One executor holds
  at most one open transaction; writers are serialized by `LockManager`
- WAL DML redo uses page images (`PageImageRedo`); DDL remains logical SQL. Legacy `PhysicalRedo`
  row after-images remain replayable. Every successful WAL append/`reset` flush+fsyncs (and syncs
  the parent directory on create on POSIX; Windows uses `FlushFileBuffers` on the WAL file only)
  so `COMMIT` / autocommit durability is not left in the OS page cache. Trailing torn WAL records
  are ignored so recovery replays the durable prefix. `SAVE DATABASE` durable-syncs the temp
  `.tcrdb` before rename and syncs the storage directory on POSIX (Windows: file sync only)
- Schema catalog changes (`CREATE DATABASE`/`TABLE`, `DROP`/`RENAME TABLE`, `ALTER TABLE`
  ADD/DROP COLUMN, `CREATE`/`DROP INDEX`)
  and `DROP DATABASE` / `SAVE`/`LOAD` follow documented txn rules: most catalog DDL is allowed with
  undo + deferred WAL; `DROP DATABASE` is rejected while a transaction is active; `SAVE`/`LOAD`
  implicitly commit / roll back
- Planner costs use live row counts, index distinct-key counts, and optional `ANALYZE` histograms
  for range/`IN` selectivity; multi-index AND intersection and top-level `OR` union (including
  partial union of indexable arms with a residual OR complementary scan) are supported
- Nested SQL is intentionally limited: `WITH` nesting deeper than depth 6 and correlation deeper
  than eight outer frames are rejected. Outer `JOIN` against a CTE/derived alias force-materializes
  the CTE. `WITH RECURSIVE` (`UNION` / `UNION ALL`, delta iteration, multiple independent recursive
  CTEs, optional `AS ACCUMULATOR` binding, safety caps) and `JOIN` inside
  `IN`/`EXISTS` are supported. Expression indexes cover column / unary minus / `+/-` literal /
  `trigram(column)` (substring `LIKE`); prefix `LIKE` uses ordered indexes; regex `~` is residual
  full-scan. `FROM` / `JOIN` table aliases and `WITH` / derived tables inside `IN`/`EXISTS` are
  supported. Parser failures report `line`/`column` via `ParseError`. Set operations include
  `UNION` / `UNION ALL` / `INTERSECT` / `INTERSECT ALL` / `EXCEPT` / `EXCEPT ALL`.
  `DROP DATABASE` clears the active database and deletes its `.tcrdb` (not allowed in an open txn).
  `EXPLAIN INSERT` reports a non-mutating insert plan summary.
- Aggregates and `GROUP BY` are supported; non-aggregated selected columns must appear in `GROUP BY`.
  Joins are left-deep `INNER` / `LEFT` / `RIGHT` / `FULL` chains and `CROSS JOIN` with `ON` `=` /
  `<` / `>` (no `ON` for `CROSS`)
- Single- and multi-column `PRIMARY KEY` / `UNIQUE`, `NOT NULL`, simple `CHECK`, and single- and
  multi-column `FOREIGN KEY` (`NO ACTION` / `CASCADE` / `SET NULL`; MATCH SIMPLE; exact parent
  UNIQUE/PK) are enforced (see [ACID Plan](docs/design.md#acid-plan))

## Roadmap

Forward-looking work lives in [docs/design.md](docs/design.md) (Next Steps and
[ACID Plan](docs/design.md#acid-plan)). Shipped milestones
include snapshot v9 multi-column FK + v8 composite UNIQUE/PK + v7 FOREIGN KEY + v6 CHECK + v5 constraint flags + page-image WAL, correlated subqueries / expression indexes / materialized CTEs,
aggregates and multi-join, histograms / multi-index AND and top-level OR union (including partial OR),
`WITH` nesting depth up to 6 and correlation through eight outer frames, `INNER`/`LEFT`/`RIGHT`/`FULL`/`CROSS` joins with
non-equi `ON`, `LIKE` / regex predicates (prefix and trigram index paths), join-table aliases, `JOIN`
inside `IN`/`EXISTS`, CTE join targets, `WITH RECURSIVE` (`UNION` / `UNION ALL`), parse diagnostics with source
positions, CI CTE + multi-index-intersect cost-shape gating, indexed `UPDATE`/`DELETE` access paths,
transactional catalog DDL (`CREATE`/`DROP INDEX`, `CREATE`/`DROP`/`RENAME TABLE`, `ALTER TABLE`
ADD/DROP COLUMN, `CREATE DATABASE`,
`DROP DATABASE`), same-column equality `OR`→`IN` rewrite, literal `IN` lists, `EXPLAIN` for
`UPDATE`/`DELETE`/`INSERT`, `EXPLAIN ANALYZE` (actual vs estimated), SI anomaly packaging with
row-level SSI write-skew / write–write aborts and insert-phantom SSI (predicate SIREAD), composite
Intersect∪Union for fully indexable nested
`OR` under `AND`, `SAVE`/`LOAD` inside open transactions (implicit commit/rollback), top-level and
CTE-body set operations (`UNION` / `UNION ALL` / `INTERSECT` / `INTERSECT ALL` / `EXCEPT` /
`EXCEPT ALL`, recursive `UNION` dedup), partial nested `OR` under `AND` (indexable arms +
complementary residual), multiple independent recursive CTEs, mutual recursion among
`WITH RECURSIVE` CTEs, `AS ACCUMULATOR` recursive binding, durable WAL `COMMIT` (flush+fsync),
single-column `PRIMARY KEY` / `UNIQUE`, first-class `NOT NULL` Consistency guarantees, durable
`SAVE DATABASE` snapshot publish, simple `CHECK` constraints, single-column `FOREIGN KEY`
(`NO ACTION` / `CASCADE` / `SET NULL`), richer predicate SIREAD for OR of column leaves and column `LIKE`, COMMIT
crash-injection durability cut points, composite indexes and multi-column `PRIMARY KEY` / `UNIQUE`
(snapshot v8), FK `ON DELETE`/`UPDATE` `CASCADE` / `SET NULL`, Phase 4 atomicity packaging
(catalog+DML failure matrix + SAVE/LOAD ACID FAQ), planner composite-index `HashEq` for
multi-equality `AND`, multi-column `FOREIGN KEY` (snapshot v9), `ALTER TABLE ADD COLUMN … NULL` /
`DROP COLUMN` (eager rewrite; dependency rejection; transactional WAL), and a dated
absolute-time benchmark summary (last refreshed
2026-08-10 from the CI `benchmark report` artifact).

Parallel product wedges: [CTE index wedge plan](docs/cte_index_wedge.md) /
[materialize vs inline comparison](docs/cte_materialize_comparison.md),
[multi-index intersect wedge](docs/multi_index_intersect_wedge.md) /
[BitmapAnd parity comparison](docs/bitmap_and_comparison.md), and
[SI anomaly concurrency wedge](docs/si_anomaly_wedge.md).

## Documentation

- [Architecture](docs/architecture.md)
- [Design status](docs/design.md) (includes [ACID Plan](docs/design.md#acid-plan))
- [SQL reference](docs/sql.md)
- [Testing](docs/testing.md)
- [Benchmarks](docs/benchmarks.md)
- [Deep features](docs/deep_features.md)
- [CTE index wedge plan](docs/cte_index_wedge.md)
- [CTE materialize vs inline comparison](docs/cte_materialize_comparison.md)
- [Multi-index intersect wedge plan](docs/multi_index_intersect_wedge.md)
- [BitmapAnd parity comparison](docs/bitmap_and_comparison.md)
- [SI anomaly concurrency wedge](docs/si_anomaly_wedge.md)

## License

MIT — see [LICENSE](LICENSE).
