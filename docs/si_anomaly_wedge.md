# SI / SSI Anomaly Concurrency Wedge

## Goal

Package VertexDB’s commit-seq MVCC **snapshot isolation** plus **row-level Serializable Snapshot
Isolation (SSI)** as a systems portfolio story:

> Under SI, VertexDB prevents dirty reads and hides commits after `BEGIN`. Row-level SSI adds
> first-committer-wins abort on overlapping read/write sets so classic write skew and write–write
> conflicts cannot both commit. Executor writers are serialized — true multi-txn interleaving is
> demonstrated at the `Table` + `TransactionManager` layer.

This is a **demo wedge**, not a claim of production concurrency control. Predicate locks / full
phantom SSI are intentionally out of scope.

## Sync model (honest)

| Layer | What it does |
| --- | --- |
| `LockManager` (per `QueryExecutor`) | SELECTs share a read lock; any writer takes an exclusive lock |
| `Table` / `Database` mutexes | Storage façade synchronization |
| MVCC + `TransactionManager` | Commit-seq snapshots; version chains; dirty-read prevention |
| Row-level SSI | Per-txn relation+row read/write sets; abort later committer on rw/ww overlap |

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
| Predicate locks / insert phantoms | **Not implemented** | Inserts outside the reader’s row read-set are not conflict-checked |

## Already done

Do not rebuild these:

1. `TransactionManager` commit sequences and `ReadSnapshot` watermarks.
2. `MVCCRowStore` / `Table` visibility (`createdBy` / `deletedBy`).
3. Executor `LockManager` reader/writer gate.
4. Existing tests: `UncommittedWritesInvisibleUntilCommit`,
   `SnapshotIsolationHidesCommitsAfterBegin`, `MVCCTracksTransactionVisibility`,
   `LockManagerAllowsSharedReadersAndBlocksWriters`, `SupportsConcurrentExecutorClients`.

## What to build

### 1. Named desired-behavior tests — done

In `tests/transaction_behavior_tests.cpp`:

- `DirtyReadOfUncommittedUpdateIsPrevented`
- `SnapshotIsolationHidesCommittedInsertMatchingPredicate`
- `SerializableSnapshotIsolationAbortsWriteSkew`
- `SerializableSnapshotIsolationAbortsWriteWriteConflict`
- `ExecutorAllowsConcurrentReadersAndWriterExcludesReaders`

### 2. Demo SQL — done

[`examples/si_isolation_demo.sql`](../examples/si_isolation_demo.sql) walks a single SQL session
through `BEGIN` / dirty-looking DML / `COMMIT` with comments pointing at the Table-level tests for
true interleaving (including SSI write-skew abort).

### 3. Wedge write-up — done

This document plus pointers from `docs/deep_features.md`, `docs/sql.md`, `docs/design.md`, and the
README.

## Demo

```sh
./build/VertexDB_cli < examples/si_isolation_demo.sql
```

For write skew / write–write SSI aborts and concurrent snapshots, run the GoogleTest cases above
(they share `TransactionManager` across two logical txns).

## Limitations (honest)

- No row/page locks, deadlock detection, or buffer-pool latches.
- No predicate locks / `FOR UPDATE`; insert phantoms outside the row read-set are not SSI-checked.
- No multi-`QueryExecutor` shared session/`TransactionManager` redesign.
- Concurrent SQL writers on one executor are serialized by `LockManager` — that is intentional for
  this educational engine, not an SI bug.

## Evidence checklist

| Artifact | Role |
|----------|------|
| [`examples/si_isolation_demo.sql`](../examples/si_isolation_demo.sql) | Single-session illustration |
| `DirtyReadOfUncommittedUpdateIsPrevented` | Dirty read prevented |
| `SnapshotIsolationHidesCommitsAfterBegin` | SI watermark / non-repeatable prevented |
| `SnapshotIsolationHidesCommittedInsertMatchingPredicate` | Mid-txn phantom prevented |
| `SerializableSnapshotIsolationAbortsWriteSkew` | Write skew aborted under row-level SSI |
| `SerializableSnapshotIsolationAbortsWriteWriteConflict` | Same-row WW aborted |
| `ExecutorAllowsConcurrentReadersAndWriterExcludesReaders` | Executor RW honesty |

## Definition of done

- [x] Named tests fail if dirty reads appear or SI watermark / held-snapshot phantoms regress.
- [x] Named tests show write skew and write–write conflicts abort the later committer.
- [x] Docs and example state prevented vs aborted anomalies without overclaiming predicate SSI.
