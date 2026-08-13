# Design Status

VertexDB is built as small, testable systems slices. The codebase includes working storage, parsing,
execution, indexing, persistence, WAL recovery, transactions, tests, benchmarks, and CI.

For module layout and data flow, see [architecture.md](architecture.md). For mechanism depth (B+ tree,
WAL, MVCC, planner costs), see [deep_features.md](deep_features.md).

## What Exists Today

- Repository foundation: CMake targets, CLI, library target, GitHub Actions CI, and documentation
- Storage engine: typed columns, nullable values, schema validation, table/database ownership,
  page-backed `RowStore`, `VectorRowStore`, `BufferPool`, index maintenance, MVCC version
  recording, and stable row IDs with tombstones plus free-list reuse
- Parser: tokenizer (token offsets / line / column), AST, grammar tests, table-management commands,
  predicates (including `LIKE` and regex `~`), ordering, limits, left-deep `INNER` / `LEFT` / `RIGHT` / `FULL` `[OUTER]` and `CROSS`
  joins with `ON col op col` (`=`, `<`, `>`; none for `CROSS`) and optional join-table aliases, aggregates/`GROUP BY`,
  `WITH` CTEs (`AS MATERIALIZED` / `AS NOT MATERIALIZED` / `AS ACCUMULATOR`, nesting depth up to 6,
  `WITH RECURSIVE` with `UNION` / `UNION ALL`, including mutual recursion), derived tables,
  `FROM` / `JOIN` table aliases, `IN`/`EXISTS` subqueries (including `WITH` / `JOIN` inside them and
  correlation through eight outer frames), outer `JOIN` against CTE/derived aliases (force
  materialize), expression indexes (including `trigram(column)`),
  `EXPLAIN` / `EXPLAIN ANALYZE`, transactions, prepared statements (typed AST + `?` slots), save/load,
  `DROP DATABASE`, exit, and
  `ParseError` diagnostics with source positions
- Query execution: projection, filtering, ordering, limit, aggregates/`GROUP BY`, insert, update,
  delete (UPDATE/DELETE `WHERE` uses the same planner index access paths as SELECT), table management,
  multi-join chains (`INNER`/`LEFT`/`RIGHT`/`FULL`/`CROSS`, equi and non-equi), CTE/derived-table
  inlining or materialization (including recursive delta or accumulator iteration, mutual
  recursion, and multiple independent recursive CTEs), set operations (`UNION` /
  `UNION ALL` / `INTERSECT` / `INTERSECT ALL` / `EXCEPT` / `EXCEPT ALL`), correlated `IN`/`EXISTS` with
  alias scopes (including joined subqueries), expression-index maintenance (including trigram),
  prepared AST binding, save/load, recovery, and transactional read routing
- Indexes: maintained hash indexes for equality lookup and ordered B+ tree index APIs for point
  and range lookup (column and expression keys), hash index `IN` multi-lookup, ordered prefix
  `LIKE`, and hash trigram indexes for substring `LIKE`
- Persistence: versioned binary snapshots (current page-payload + index-pages v4; page-payload v3,
  sparse v2, and dense v1 still readable) under `.tcrdb` files, with `tcrdb_codec` owning the layout
- WAL and recovery: append-only WAL with page-image redo for DML (legacy physical row-image redo
  still replayable), logical SQL for DDL, flush+fsync on every successful append/`reset` (durable
  `COMMIT` / autocommit), truncated-trailing-record tolerance, startup replay, save checkpoints,
  and crash-simulation tests
- Concurrency: executor-level reader/writer synchronization (`LockManager`) and concurrent client
  tests; SI/SSI anomaly evidence packaged in [si_anomaly_wedge.md](si_anomaly_wedge.md)
- Transactions: commit-aware MVCC snapshot isolation (dirty reads / SI watermark / mid-txn phantoms
  prevented) plus SSI commit aborts for write skew, write–write conflicts, and insert phantoms
  (predicate SIREAD vs insert/update images), undo-log DML rollback, transaction-batched page-image
  WAL flush+fsync on `COMMIT`
- Planner: cost-based access paths (including multi-index AND intersect, top-level OR union with
  partial residual OR, composite Intersect∪Union for nested OR under AND including partial nested
  OR, prefix `LIKE`, and trigram intersect), residual filters, join algorithm
  selection (`INNER`/`LEFT`/`RIGHT`/`FULL`/`CROSS`, equi and non-equi), expression-index matching,
  `EXPLAIN`, and `EXPLAIN ANALYZE` (actual vs estimated rows)
- Quality: themed GoogleTest suites, regression tests, sanitizer/coverage scripts, benchmarks with
  a CI CTE cost-shape gate, CI

## Known Limitations

- Snapshot isolation prevents dirty reads and hides commits after `BEGIN`. SSI aborts a later
  committer on overlapping row read/write sets (write skew / write–write) or on insert-phantom
  conflicts (predicate SIREAD summaries vs inserted or update-produced row images). There are no
  row/page locks or Postgres-style next-key locks; OR/LIKE/subquery scans use conservative
  relation-membership SIREADs. See [si_anomaly_wedge.md](si_anomaly_wedge.md).
- Integrity constraints beyond typed/nullable columns are not implemented yet; the path to stronger
  ACID **C** (and optional I/D polish) is in [ACID Plan](#acid-plan).
- `SAVE DATABASE` in an open transaction implicitly commits then checkpoints; `LOAD DATABASE`
  implicitly rolls back then loads.
- DML WAL redo stores page images (`PageImageRedo`); DDL still uses logical SQL payloads. Legacy
  `PhysicalRedo` and logical `Insert`/`Update`/`Delete` records remain replayable for old WALs.
- Top-level `OR` of equality (or expression-equality) index probes uses multi-index union when the
  indexable subset is cheaper than a full scan. Non-indexable disjuncts become a residual OR
  complementary scan (partial OR). When no disjunct is indexable, or the indexable union is not
  cheaper than a scan, the planner keeps a full scan. Same-column equality `OR` (top-level or under
  `AND`) is rewritten to `IN` for HashIn. A heterogeneous `OR` nested under `AND` whose equality-
  indexable arms are non-empty becomes a Union child of a multi-index Intersect (composite
  Intersect∪Union) when that plan beats the best single conjunct; non-indexable arms become a
  complementary residual under the outer AND (partial nested OR). If no nested-OR arm is
  equality-indexable, the whole `OrPred` stays an AND residual.
- Nested SQL is limited: `WITH` nesting deeper than depth 6 and correlation deeper than eight outer
  frames are rejected. Supported nested forms include `WITH` nesting depth up to 6, `WITH` /
  derived tables and `JOIN` inside `IN`/`EXISTS`, outer `JOIN` against a CTE/derived alias (force
  materialize), `WITH RECURSIVE` (`UNION` / `UNION ALL`, delta or `AS ACCUMULATOR` binding, multiple
  independent or mutually recursive CTEs, 1000-iteration / 100000-row caps), `FROM` / `JOIN`
  table aliases (`AS` optional) for qualification and
  correlation scopes, and expression indexes (`column`, `-column`, `column+/-literal`,
  `trigram(column)`). CTE/derived bodies may include left-deep `INNER` / `LEFT` / `RIGHT` /
  `FULL` / `CROSS` joins. Parser/tokenizer failures report `line`/`column` source positions via
  `ParseError`. Set operations (`UNION` / `UNION ALL` / `INTERSECT` / `INTERSECT ALL` / `EXCEPT` /
  `EXCEPT ALL`) are left-associative outside and inside CTE bodies; recursive CTEs accept bare
  `UNION` (dedup) or `UNION ALL`.
- Aggregates/`GROUP BY` are supported; joins are left-deep `INNER` / `LEFT` / `RIGHT` / `FULL`
  `[OUTER]` and `CROSS` chains (`ON col op col` for non-`CROSS`). Catalog DDL includes
  `CREATE`/`DROP DATABASE`, table/index commands, and `RENAME TABLE`; broader ALTER-style DDL is
  still out of scope.

## Next Steps

Shipped recently (no longer open work): literal `IN` lists, indexed `UPDATE`/`DELETE` access paths,
transactional `CREATE INDEX` / `DROP INDEX`, same-column `OR`→`IN`, MVCC `UPDATE` close-prior-version,
`EXPLAIN ANALYZE` (actual vs estimated rows for SELECT/WITH), `EXPLAIN UPDATE`/`DELETE`, multi-index
AND intersect packaging, the SI anomaly concurrency wedge, composite Intersect∪Union for fully
indexable nested `OR` under `AND`, transactional catalog DDL, `SAVE`/`LOAD` inside open
transactions (implicit commit / rollback), set operations (`UNION` / `UNION ALL` / `INTERSECT` /
`EXCEPT`, including recursive `UNION` dedup), partial nested `OR` under `AND` (indexable
arms + complementary residual), multiple independent recursive CTEs, `INTERSECT ALL` /
`EXCEPT ALL`, mutual recursion among `WITH RECURSIVE` CTEs, `AS ACCUMULATOR` binding
for the recursive arm, row-level SSI commit aborts for write skew / write–write conflicts,
insert-phantom SSI (predicate SIREAD vs insert/update images), `DROP DATABASE`, raised
`WITH`/correlation caps (6 / 8), `EXPLAIN INSERT`, and durable WAL `COMMIT` (flush+fsync on
append/`reset`).

Forward-looking options (intentional gaps, pick by teaching value):

- **ACID alignment** — see [ACID Plan](#acid-plan) below (constraints first; then optional deeper I/D)
- Maintenance: re-refresh absolute times in [benchmarks.md](benchmarks.md) after planner/storage
  changes that stale the 2026-08-10 table (include intersect benches); wedge **cost shape** already
  gates every push/PR via `scripts/run-benchmarks.sh --check-shape`
- ALTER-style DDL (`ADD`/`DROP COLUMN`, etc.) — orthogonal to ACID; useful catalog teaching

Demo wedges (done):

- CTE inlining so outer predicates hit base-table indexes:
  [cte_index_wedge.md](cte_index_wedge.md), [cte_materialize_comparison.md](cte_materialize_comparison.md)
- Multi-index AND intersect vs single-index residual:
  [multi_index_intersect_wedge.md](multi_index_intersect_wedge.md),
  [bitmap_and_comparison.md](bitmap_and_comparison.md)
- Snapshot isolation anomalies (prevented vs allowed) and executor RW honesty:
  [si_anomaly_wedge.md](si_anomaly_wedge.md)

## ACID Plan

VertexDB should **lean into ACID as a teaching north star**: clear, testable guarantees with honest
docs — not a claim of production Postgres/InnoDB parity. Prefer small slices that close a named gap
with desired-behavior tests over vague “more ACID” churn.

### Scorecard (today)

| Property | Status | What exists | Main gap |
| --- | --- | --- | --- |
| **A** Atomicity | Strong | Undo-log `ROLLBACK`; deferred WAL dropped on abort; txn DML flushed as one batch on `COMMIT`; invalid multi-row insert refuses without partial WAL | Rare edge cases around catalog DDL + crash mid-`SAVE` publish remain educational |
| **C** Consistency | Weak | Typed schema, nullability checks | No `PRIMARY KEY` / `UNIQUE` / `FOREIGN KEY` / `CHECK`; “consistent” mostly means “well-typed rows” |
| **I** Isolation | Strong (educational) | Commit-seq MVCC SI; SSI aborts for write skew, write–write, insert phantoms (predicate SIREAD); executor `LockManager` | One open SQL txn per executor; OR/LIKE/subquery use relation-membership SIREAD fallbacks; no next-key / gap locks |
| **D** Durability | Strong (educational) | WAL flush+fsync (`F_FULLFSYNC` on macOS when available) on append/`reset`; torn trailing record ignored; startup replay | Parent-directory sync is POSIX-only (Windows: file `FlushFileBuffers`); snapshot `.tcrdb` publish is rename-based without directory fsync; power-loss models are not exhaustive |

### Principles

1. **Document the contract** in [sql.md](sql.md) / [architecture.md](architecture.md) before or with the code.
2. **Test desired behavior** (named tests that would fail before the change) — see [testing.md](testing.md).
3. **Keep the façade split**: parser → catalog/DML/select engines → `Table` / `WriteAheadLog` / `TransactionManager`; do not grow a second transaction story inside the CLI.
4. **Prefer teaching value** over completeness: one well-explained constraint beats five half-wired ones.
5. **Do not encode today’s bugs as golden ACID claims** — if a guarantee is incomplete, say so in the scorecard and tests.

### Phased work

#### Phase 1 — Consistency (highest teaching leverage)

Ship integrity constraints so `C` is engine-enforced, not only application convention.

| Slice | Scope | Touch points | Done when |
| --- | --- | --- | --- |
| **1a. `PRIMARY KEY` / `UNIQUE`** | Column or simple column-list uniqueness; reject duplicate `INSERT`/`UPDATE`; optional unique hash/B+ index maintenance | Parser DDL, `Table` / `IndexManager`, DML engine, `.tcrdb` schema metadata, SQL docs | Desired tests: duplicate insert/update aborted; unique index used for equality; save/load preserves constraint |
| **1b. `NOT NULL` as first-class constraint story** | Already partially present via nullable columns — document and test as an ACID-`C` guarantee; align error messages | Docs + any missing DML rejection tests | Explicit tests named for null-rejection on non-nullable columns (insert/update) |
| **1c. `CHECK` (simple)** | Boolean predicate on row image at insert/update (column comparisons / AND / OR; no subqueries v1) | Parser, DML validate path, snapshot metadata | Rejecting insert/update tests; `EXPLAIN` optional |
| **1d. `FOREIGN KEY` (optional later)** | Same-database parent lookup on insert/update; restrict or reject delete of referenced parent | Catalog + DML; careful interaction with SI visibility | Only after 1a; document ON DELETE/UPDATE policy (start with `NO ACTION` / reject) |

Defer: deferred constraint checking, exclusion constraints, domain types.

#### Phase 2 — Isolation polish (only if teaching SSI deeper)

Already covered for the portfolio wedge in [si_anomaly_wedge.md](si_anomaly_wedge.md). Further work is optional:

| Slice | Scope | Notes |
| --- | --- | --- |
| **2a. Richer predicate SIREAD** | Replace relation-membership fallbacks for a documented subset of OR/LIKE with real column predicates | Keep honesty about what still falls back |
| **2b. Multi-txn SQL demos** | Still one txn per `QueryExecutor`; keep interleaving tests at `Table` + `TransactionManager` unless a multi-session façade is explicitly desired | Do not pretend the CLI is multi-writer concurrent |

Non-goal unless product direction changes: full 2PL, Postgres next-key locks, or true multi-master concurrency.

#### Phase 3 — Durability hardening (incremental)

Durable `COMMIT` via WAL sync is shipped. Optional follow-ups:

| Slice | Scope | Notes |
| --- | --- | --- |
| **3a. Durable `SAVE DATABASE`** | fsync snapshot temp file + parent directory before/after rename (mirror WAL discipline) | Closes “checkpoint lost on crash” teaching hole |
| **3b. Group commit / sync policy** | Optional `WalDurability::{Sync,FlushOnly}` for benchmarks — default remains Sync | Only if bench noise from fsync becomes a problem; never silently weaken default |
| **3c. Crash-injection tests** | Kill after WAL sync / before in-memory commit mark; assert recovery | Strengthens D without new features |

#### Phase 4 — Atomicity edge polish

| Slice | Scope |
| --- | --- |
| **4a. Catalog + DML failure matrix** | Table of which DDL/DML combinations are atomic across crash vs `ROLLBACK` (already mostly true; make it a doc + test checklist) |
| **4b. `SAVE` / `LOAD` vs open txn** | Keep implicit commit/rollback; add a short ACID FAQ so users do not read them as nested transactions |

### Suggested order of attack

1. Phase **1a** (`PRIMARY KEY` / `UNIQUE`) — largest `C` gap, natural index tie-in.
2. Phase **1b** docs/tests for `NOT NULL` as an explicit consistency guarantee.
3. Phase **3a** durable snapshot publish — pairs well with the recent WAL fsync work.
4. Phase **1c** simple `CHECK` if constraint teaching is still the focus.
5. Phase **2a** / **3c** only when packaging a deeper concurrency or recovery story.

### Explicit non-goals (for now)

- Claiming “full ACID” or “serializable like Postgres” in the README without the scorecard caveats.
- Row/page locks as the primary isolation mechanism (MVCC + SSI remains the story).
- Distributed transactions, 2PC, or replication.
- Weakening durable WAL sync to chase benchmark absolute times (use shape gates / optional policy instead).

### ACID definition of done (per slice)

Each ACID slice must update this scorecard, ship focused tests, and keep [sql.md](sql.md) / README limitation language honest. Prefer one merged slice over a half-finished constraint grammar.

## Definition of Done

Each feature should include:

- Public interface and implementation.
- Unit or execution tests for desired behavior and edge cases.
- Regression tests for bugs and non-obvious failure modes.
- SQL documentation updates when syntax or behavior changes.
- Benchmark updates when performance-sensitive paths change.
- Clean `ctest`, sanitizer, and coverage runs before merging.
