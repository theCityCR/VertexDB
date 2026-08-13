# Design Status

VertexDB is built as small, testable systems slices. The codebase includes working storage, parsing,
execution, indexing, persistence, WAL recovery, transactions, tests, benchmarks, and CI.

For module layout and data flow, see [architecture.md](architecture.md). For mechanism depth (B+ tree,
WAL, MVCC, planner costs), see [deep_features.md](deep_features.md).

## What Exists Today

- Repository foundation: CMake targets, CLI, library target, GitHub Actions CI, and documentation
- Storage engine: typed columns, nullable values, single- and multi-column `PRIMARY KEY` / `UNIQUE`
  constraints (auto hash/B+ indexes including composite keys; duplicate `INSERT`/`UPDATE` rejected), schema validation,
  table/database ownership, page-backed `RowStore`, `VectorRowStore`, `BufferPool`, index
  maintenance, MVCC version recording, and stable row IDs with tombstones plus free-list reuse
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
  and range lookup (column, composite column, and expression keys), hash index `IN` multi-lookup,
  ordered prefix `LIKE`, and hash trigram indexes for substring `LIKE`
- Persistence: versioned binary snapshots (current page-payload + index-pages + column constraint
  flags + CHECK + FOREIGN KEY + table-level composite UNIQUE/PK v8; FK v7, CHECK v6, constraint
  flags v5, index-pages v4, page-payload v3, sparse v2, and dense v1 still readable) under `.tcrdb`
  files, with `tcrdb_codec` owning the layout; durable `SAVE` (fsync temp snapshot + parent
  directory sync on POSIX, then rename)
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
  row/page locks or Postgres-style next-key locks. Regex / subquery / expression-index probes use
  conservative relation-membership SIREADs; OR of column comparisons/`IN`/`LIKE` and column `LIKE`
  record real column predicates. See [si_anomaly_wedge.md](si_anomaly_wedge.md).
- Integrity constraints: single-column and multi-column `PRIMARY KEY` / `UNIQUE` (composite indexes;
  auto `__pk_*` / `__uq_*`), `NOT NULL` (default columns; explicit keyword supported), simple
  `CHECK` (column comparisons with `AND`/`OR`), and single-column `FOREIGN KEY` (`REFERENCES` /
  `FOREIGN KEY … REFERENCES`, `ON DELETE`/`UPDATE` `NO ACTION` / `CASCADE` / `SET NULL`) are
  enforced. Multi-column FK is not implemented yet; see [ACID Plan](#acid-plan).
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
`WITH`/correlation caps (6 / 8), `EXPLAIN INSERT`, durable WAL `COMMIT` (flush+fsync on
append/`reset`), single-column `PRIMARY KEY` / `UNIQUE` (snapshot v5 constraint flags), and
first-class `NOT NULL` Consistency messaging/tests, durable `SAVE DATABASE` snapshot publish
(fsync temp + POSIX directory sync), simple `CHECK` constraints (column comparisons with
`AND`/`OR`; snapshot v6), single-column `FOREIGN KEY` (`NO ACTION`; snapshot v7), richer
predicate SIREAD for OR of column leaves and column `LIKE` (regex / subquery / expression-index
probes still relation-membership), COMMIT crash-injection cut points (after WAL sync / before
commit mark, plus before WAL sync), composite indexes plus multi-column `PRIMARY KEY` /
`UNIQUE` (snapshot v8 table-level constraints + `cols:…` index metadata), and FK
`ON DELETE`/`UPDATE` `CASCADE` / `SET NULL` (with `NO ACTION` still the default).

Shipped (Phase 4): catalog + DML [atomicity matrix](#phase-4--atomicity-edge-polish) with named
test checklist; [ACID FAQ](sql.md#acid-faq-save--load-vs-transactions) for implicit `SAVE`/`LOAD`
(not nested transactions).

Forward-looking options (intentional gaps, pick by teaching value):

- **ACID alignment** — see [ACID Plan](#acid-plan) below (optional group-commit sync policy;
  multi-column FK)
- Maintenance: re-refresh absolute times in [benchmarks.md](benchmarks.md) after planner/storage
  changes that stale the 2026-08-10 table (include intersect benches); wedge **cost shape** already
  gates every push/PR via `scripts/run-benchmarks.sh --check-shape`
- ALTER-style DDL (`ADD`/`DROP COLUMN`, etc.) — orthogonal to ACID; useful catalog teaching
- Planner use of composite indexes for multi-equality `AND` (vs Intersect of single-column indexes)

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
| **A** Atomicity | Strong | Undo-log `ROLLBACK`; deferred WAL dropped on abort; txn DML flushed as one batch on `COMMIT`; invalid multi-row insert refuses without partial WAL; catalog+DML [failure matrix](#phase-4--atomicity-edge-polish) + SAVE/LOAD [ACID FAQ](sql.md#acid-faq-save--load-vs-transactions) | Crash mid-`SAVE` rename remains an educational durability edge (POSIX dir sync after rename); not a nested-txn story |
| **C** Consistency | Strong (educational) | Typed schema; `NOT NULL` (default) / `NULL`; single- and multi-column `PRIMARY KEY` / `UNIQUE` (composite indexes); simple `CHECK`; single-column `FOREIGN KEY` (`NO ACTION` / `CASCADE` / `SET NULL`) | Multi-column FK not yet |
| **I** Isolation | Strong (educational) | Commit-seq MVCC SI; SSI aborts for write skew, write–write, insert phantoms (predicate SIREAD including OR of column leaves and column LIKE); executor `LockManager` | One open SQL txn per executor; regex / subquery / expression-index probes use relation-membership SIREAD fallbacks; no next-key / gap locks |
| **D** Durability | Strong (educational) | WAL flush+fsync (`F_FULLFSYNC` on macOS when available) on append/`reset`; durable `SAVE` (fsync temp `.tcrdb` + POSIX directory sync around rename); torn trailing WAL ignored; startup replay; crash-injection at COMMIT cut points | Parent-directory sync is POSIX-only (Windows: file `FlushFileBuffers`); power-loss models are not exhaustive |

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
| **1a. `PRIMARY KEY` / `UNIQUE`** | Column uniqueness; reject duplicate `INSERT`/`UPDATE`; auto `__pk_`/`__uq_` hash/B+ indexes; snapshot v5 flags | Parser DDL, `Table` / `IndexManager`, DML engine, `.tcrdb` schema metadata, SQL docs | **Done** — duplicate insert/update aborted; unique index used for equality; save/load preserves constraint |
| **1a2. Multi-column `PRIMARY KEY` / `UNIQUE`** | Table-level `PRIMARY KEY (…)` / `UNIQUE (…)`; composite indexes; snapshot v8 | Parser, `IndexManager` composite keys, `UniqueConstraint`, `.tcrdb` v8 | **Done** — composite equality keys; `__pk_a_b` / `__uq_a_b`; NULL parts distinct for UNIQUE |
| **1b. `NOT NULL` as first-class constraint story** | Document and test null-rejection as an ACID-`C` guarantee; aligned error messages name the column | Docs + DML rejection tests; WAL `createTableSql` emits `NOT NULL` | **Done** — named insert/update/multi-row tests; `NOT NULL constraint violation on column …` |
| **1c. `CHECK` (simple)** | Boolean predicate on row image at insert/update (column comparisons / AND / OR; no subqueries v1) | Parser, DML validate path, snapshot metadata | **Done** — rejecting insert/update tests; UNKNOWN (NULL) accepted; save/load v6 |
| **1d. `FOREIGN KEY`** | Same-database parent lookup on insert/update; `NO ACTION` / `CASCADE` / `SET NULL` | Catalog + DML; SI-visible parent/child probes; SET NULL requires nullable child | **Done** — reject / cascade erase / null child FK; DROP/RENAME parent still rejected while referenced |

Defer: deferred constraint checking, exclusion constraints, domain types.

#### Phase 2 — Isolation polish (only if teaching SSI deeper)

Already covered for the portfolio wedge in [si_anomaly_wedge.md](si_anomaly_wedge.md). Further work is optional:

| Slice | Scope | Notes |
| --- | --- | --- |
| **2a. Richer predicate SIREAD** | Replace relation-membership fallbacks for a documented subset of OR/LIKE with real column predicates | **Done** — OR of column comparisons/`IN`/`LIKE` records each arm; column `LIKE` matches via `matchLikePattern`; regex / subquery / expression-index probes still fall back |
| **2b. Multi-txn SQL demos** | Still one txn per `QueryExecutor`; keep interleaving tests at `Table` + `TransactionManager` unless a multi-session façade is explicitly desired | Do not pretend the CLI is multi-writer concurrent |

Non-goal unless product direction changes: full 2PL, Postgres next-key locks, or true multi-master concurrency.

#### Phase 3 — Durability hardening (incremental)

Durable `COMMIT` via WAL sync is shipped. Optional follow-ups:

| Slice | Scope | Notes |
| --- | --- | --- |
| **3a. Durable `SAVE DATABASE`** | fsync snapshot temp file + parent directory before/after rename (shared `durable_sync` with WAL) | **Done** — temp fsync before rename; POSIX dir sync after; `SaveDatabasePerformsDurablePublish` |
| **3b. Group commit / sync policy** | Optional `WalDurability::{Sync,FlushOnly}` for benchmarks — default remains Sync | Only if bench noise from fsync becomes a problem; never silently weaken default |
| **3c. Crash-injection tests** | Kill after WAL sync / before in-memory commit mark; assert recovery | **Done** — `QueryExecutor::armCrashInjection`; `RecoverSurvivesCrashAfterWalSyncBeforeCommitMark` + complementary `CrashBeforeWalSyncDoesNotDurableCommit` |

#### Phase 4 — Atomicity edge polish

| Slice | Scope | Done when |
| --- | --- | --- |
| **4a. Catalog + DML failure matrix** | Table of which DDL/DML combinations are atomic across crash vs `ROLLBACK` (already mostly true; make it a doc + test checklist) | **Done** — matrix below; checklist tests named in the Test column |
| **4b. `SAVE` / `LOAD` vs open txn** | Keep implicit commit/rollback; add a short ACID FAQ so users do not read them as nested transactions | **Done** — [ACID FAQ](sql.md#acid-faq-save--load-vs-transactions); `SaveDatabaseInTransactionCommitsThenCheckpoints` / `LoadDatabaseInTransactionRollsBackThenLoads` |

**Catalog + DML atomicity matrix** (educational contract; one open SQL txn per executor):

| Operation | In open txn? | On `ROLLBACK` | After `COMMIT` + restart (WAL) | Primary tests |
| --- | --- | --- | --- | --- |
| `INSERT` / `UPDATE` / `DELETE` | Yes (deferred page-image redo) | Undo LIFO; deferred WAL dropped | Batch replayed | `RollbackDropsDeferredWalRecords`, `CommitReturnsOnlyAfterDeferredWalIsDurable`, crash-injection pair |
| Invalid multi-row `INSERT` | Autocommit or in txn | No partial row write / no DML WAL | n/a (refused) | `InvalidMultiRowInsertIsAtomicAndDoesNotWriteWalRecord` (+ constraint multi-row refusals) |
| `CREATE TABLE` | Yes | Table removed | Logical SQL replayed | `CreateTableRollbackRemovesTable`, `CreateTableCommitFlushesWalAndRecovers` |
| `DROP TABLE` | Yes | Table + rows + indexes restored | Logical SQL replayed | `DropTableRollbackRestoresTableAndRows`, `DropTableCommitFlushesWalAndRecovers` |
| `RENAME TABLE` | Yes | Prior name restored; undo/pending WAL remounted | Logical SQL replayed | `RenameTableRollbackRestoresNameAndPendingDml`, `RenameTableCommitFlushesWalAndRecovers` |
| `CREATE INDEX` / `DROP INDEX` | Yes | Index removed / restored | Logical SQL replayed | `CreateIndexRollbackRemovesIndex`, `DropIndexRollbackRestoresIndex`, matching `*CommitFlushesWalAndRecovers` |
| `CREATE DATABASE` | Yes (swaps instance) | Prior database restored | Autocommit path also durable | `CreateDatabaseRollbackRestoresPriorDatabase` |
| `DROP DATABASE` | **Rejected** | n/a | Autocommit only; WAL + snapshot delete | `DropDatabaseClearsActiveAndDeletesSnapshot` (in-txn reject), `DropDatabaseWalRecoversWithoutActiveDatabase` |
| Mixed catalog + DML in one txn | Yes | Both undone | Both durable after commit | `CreateIndexWithInsertRollbackRestoresBoth`, `CatalogAndDmlMixedCommitFlushesWalAndRecovers` |
| `SAVE DATABASE` | Implicit `COMMIT` then checkpoint | n/a (txn already closed) | Snapshot + WAL checkpoint | `SaveDatabaseInTransactionCommitsThenCheckpoints` |
| `LOAD DATABASE` | Implicit `ROLLBACK` then load | Uncommitted work discarded | Loads snapshot (not nested txn) | `LoadDatabaseInTransactionRollsBackThenLoads` |

Crash cut points for DML `COMMIT`: after WAL sync / before in-memory commit mark → durable; before WAL sync → not durable (`RecoverSurvivesCrashAfterWalSyncBeforeCommitMark`, `CrashBeforeWalSyncDoesNotDurableCommit`).

### Suggested order of attack

1. Optional planner preference for composite indexes on multi-equality `AND`.
2. Multi-column `FOREIGN KEY` only if teaching composite referential integrity is the focus.

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
