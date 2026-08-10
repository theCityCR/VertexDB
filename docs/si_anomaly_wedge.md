# SI Anomaly Concurrency Wedge

## Goal

Package VertexDB’s existing commit-seq MVCC **snapshot isolation** as a systems portfolio story:

> Under snapshot isolation, VertexDB prevents dirty reads and hides commits after `BEGIN`; classic SI
> still allows write skew. Executor writers are serialized — true multi-txn interleaving is
> demonstrated at the `Table` + `TransactionManager` layer.

This is a **demo wedge**, not a claim of production concurrency control. The engine behavior already
exists; this plan packages evidence so the isolation story is repeatable and honest.

## Sync model (honest)

| Layer | What it does |
| --- | --- |
| `LockManager` (per `QueryExecutor`) | SELECTs share a read lock; any writer takes an exclusive lock |
| `Table` / `Database` mutexes | Storage façade synchronization |
| MVCC + `TransactionManager` | Commit-seq snapshots; version chains; dirty-read prevention |

One `QueryExecutor` holds **at most one** open SQL transaction. Tests that interleave two logical
transactions share a `Table` and `TransactionManager` directly (same pattern as existing SI tests).

## Anomaly table

| Anomaly | Under VertexDB SI | Evidence |
| --- | --- | --- |
| Dirty read | **Prevented** | Uncommitted INSERT/UPDATE invisible to other snapshots |
| Non-repeatable read | **Prevented** | Commits after `BEGIN` stay behind `maxCommitSeq` |
| Mid-txn phantom (same snapshot) | **Prevented** | Predicate-matching insert+commit invisible to held snapshot |
| Write skew | **Allowed** (SI contract) | Two txns each flip a different “on-call” row; invariant breaks |
| Predicate locks / SSI abort | **Not implemented** | Concurrent inserts are not conflict-checked |

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
- `SnapshotIsolationAllowsWriteSkew` (documented SI limitation)
- `ExecutorAllowsConcurrentReadersAndWriterExcludesReaders`

### 2. Demo SQL — done

[`examples/si_isolation_demo.sql`](../examples/si_isolation_demo.sql) walks a single SQL session
through `BEGIN` / dirty-looking DML / `COMMIT` with comments pointing at the Table-level tests for
true interleaving.

### 3. Wedge write-up — done

This document plus pointers from `docs/deep_features.md`, `docs/sql.md`, `docs/design.md`, and the
README.

## Demo

```sh
./build/VertexDB_cli < examples/si_isolation_demo.sql
```

For write skew and concurrent snapshots, run the GoogleTest cases above (they share
`TransactionManager` across two logical txns).

## Limitations (honest)

- No row/page locks, deadlock detection, or buffer-pool latches.
- No Serializable Snapshot Isolation (SSI), `FOR UPDATE`, or write–write abort.
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
| `SnapshotIsolationAllowsWriteSkew` | Write skew allowed under SI |
| `ExecutorAllowsConcurrentReadersAndWriterExcludesReaders` | Executor RW honesty |

## Definition of done

- [x] Named tests fail if dirty reads appear or SI watermark / held-snapshot phantoms regress.
- [x] Named test documents write skew as an intentional SI allowance.
- [x] Docs and example state prevented vs allowed anomalies without overclaiming.
