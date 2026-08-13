# SI / SSI Anomaly Concurrency Wedge

## Goal

Package VertexDB’s commit-seq MVCC **snapshot isolation** plus **Serializable Snapshot Isolation
(SSI)** as a systems portfolio story:

> Under SI, VertexDB prevents dirty reads and hides commits after `BEGIN`. SSI adds
> first-committer-wins abort on overlapping row read/write sets (write skew / write–write) and on
> insert-phantom conflicts: predicate SIREAD summaries vs inserted or update-produced row images
> (column comparisons, `IN` lists, OR of those leaves, and column `LIKE`; relation membership for
> regex / subquery / expression-index probes). Executor writers are serialized — true multi-txn
> interleaving is demonstrated at the `Table` + `TransactionManager` layer.

This is a **demo wedge**, not a claim of production concurrency control. Full Postgres-style
next-key / gap locks are out of scope; predicate coverage is educational (simple column comparisons,
`IN` lists, OR of column leaves, column `LIKE`, and conservative relation membership for
regex/subquery/expression shapes).

## Sync model (honest)

| Layer | What it does |
| --- | --- |
| `LockManager` (per `QueryExecutor`) | SELECTs share a read lock; any writer takes an exclusive lock |
| `Table` / `Database` mutexes | Storage façade synchronization |
| MVCC + `TransactionManager` | Commit-seq snapshots; version chains; dirty-read prevention |
| Row-level SSI | Per-txn relation+row read/write sets; abort later committer on rw/ww overlap |
| Insert-phantom SSI | Predicate reads (incl. empty probes) retained across commit; conflict with matching inserts/updates |

One `QueryExecutor` holds **at most one** open SQL transaction. Tests that interleave two logical
transactions share a `Table` and `TransactionManager` directly (same pattern as existing SI tests).

## Anomaly table

| Anomaly | Under VertexDB | Evidence |
| --- | --- | --- |
| Dirty read | **Prevented** (SI) | Uncommitted INSERT/UPDATE invisible to other snapshots |
| Non-repeatable read | **Prevented** (SI) | Commits after `BEGIN` stay behind `maxCommitSeq` |
| Mid-txn phantom (same snapshot) | **Prevented** (SI watermark) | Predicate-matching insert+commit invisible to held snapshot |
| Write skew | **Aborted** (row-level SSI) | Two txns each flip a different “on-call” row; later `COMMIT` fails |
| Write–write on same row | **Aborted** (row-level SSI) | First committer wins |
| Insert phantom / empty probe | **Aborted** (predicate SSI) | Matching insert or update-into-predicate vs SIREAD; later committer fails |

## Already done

Do not rebuild these:

1. `TransactionManager` commit sequences and `ReadSnapshot` watermarks.
2. `MVCCRowStore` / `Table` visibility (`createdBy` / `deletedBy`).
3. Executor `LockManager` reader/writer gate.
4. Row-level SSI write skew / write–write aborts.
5. Insert-phantom SSI (`recordPredicateRead` / `recordInsert`; SelectEngine scan recording).
6. Existing tests: `UncommittedWritesInvisibleUntilCommit`,
   `SnapshotIsolationHidesCommitsAfterBegin`, `MVCCTracksTransactionVisibility`,
   `LockManagerAllowsSharedReadersAndBlocksWriters`, `SupportsConcurrentExecutorClients`,
   plus the SSI tests listed below.

## What to build

### 1. Named desired-behavior tests — done

In `tests/transaction_behavior_tests.cpp`:

- `DirtyReadOfUncommittedUpdateIsPrevented`
- `SnapshotIsolationHidesCommittedInsertMatchingPredicate`
- `SerializableSnapshotIsolationAbortsInsertPhantom`
- `SerializableSnapshotIsolationAbortsInsertPhantomEmptyProbe`
- `SerializableSnapshotIsolationAbortsUpdateIntoPredicate`
- `SerializableSnapshotIsolationAbortsWriteSkew`
- `SerializableSnapshotIsolationAbortsWriteWriteConflict`
- `ExecutorAllowsConcurrentReadersAndWriterExcludesReaders`

### 2. Demo SQL — done

[`examples/si_isolation_demo.sql`](../examples/si_isolation_demo.sql) walks a single SQL session
through `BEGIN` / dirty-looking DML / `COMMIT` with comments pointing at the Table-level tests for
true interleaving (including SSI write-skew and insert-phantom aborts).

### 3. Wedge write-up — done

This document plus pointers from `docs/deep_features.md`, `docs/sql.md`, `docs/design.md`, and the
README.

## Demo

```sh
./build/VertexDB_cli < examples/si_isolation_demo.sql
```

For write skew / write–write / insert-phantom SSI aborts and concurrent snapshots, run the GoogleTest
cases above (they share `TransactionManager` across two logical txns).

## Limitations (honest)

- No row/page locks, deadlock detection, or buffer-pool latches.
- Predicate SSI covers simple column comparisons, `IN` lists, OR of those column leaves, and
  column `LIKE` (pattern match via `matchLikePattern`). Regex / subquery / expression-index
  probes (including trigram) still take a conservative relation-membership SIREAD (any insert into
  the table conflicts).
- No `FOR UPDATE`; no multi-`QueryExecutor` shared session/`TransactionManager` redesign.
- Concurrent SQL writers on one executor are serialized by `LockManager` — that is intentional for
  this educational engine, not an SI bug.

## Evidence checklist

| Artifact | Role |
|----------|------|
| [`examples/si_isolation_demo.sql`](../examples/si_isolation_demo.sql) | Single-session illustration |
| `DirtyReadOfUncommittedUpdateIsPrevented` | Dirty read prevented |
| `SnapshotIsolationHidesCommitsAfterBegin` | SI watermark / non-repeatable prevented |
| `SnapshotIsolationHidesCommittedInsertMatchingPredicate` | Mid-txn phantom prevented (SI) |
| `SerializableSnapshotIsolationAbortsInsertPhantom` | Insert phantom aborted under predicate SSI |
| `SerializableSnapshotIsolationAbortsInsertPhantomEmptyProbe` | Empty probe SIREAD vs later insert |
| `SerializableSnapshotIsolationAbortsUpdateIntoPredicate` | Update-into-range treated as insert image |
| `SerializableSnapshotIsolationAbortsWriteSkew` | Write skew aborted under row-level SSI |
| `SerializableSnapshotIsolationAbortsWriteWriteConflict` | Same-row WW aborted |
| `OrPredicateSireadAbortsMatchingInsertOnly` | OR column SIREADs (matching abort / non-match ok) |
| `LikePredicateSireadAbortsMatchingInsertOnly` | LIKE column SIREAD (matching abort / non-match ok) |
| `SelectEngineOrLikeScanRecordsColumnSireads` | SelectEngine records OR/LIKE column SIREADs |
| `RegexSubqueryPredicateReadsUseRelationMembershipSiread` | Documented membership fallback |
| `ExecutorAllowsConcurrentReadersAndWriterExcludesReaders` | Executor RW honesty |

## Definition of done

- [x] Named tests fail if dirty reads appear or SI watermark / held-snapshot phantoms regress.
- [x] Named tests show write skew and write–write conflicts abort the later committer.
- [x] Named tests show insert phantoms (including empty probes and update-into-predicate) abort.
- [x] Docs and example state prevented vs aborted anomalies without overclaiming Postgres gap locks.
