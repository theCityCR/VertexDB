# Deep Features

## B+ Tree Index

The first implementation exposes B+ tree behavior through an ordered index API:

- point lookup
- less-than range lookup
- greater-than range lookup
- ordered row id storage per key

Internally it maintains explicit B+ tree layout metadata: leaf page ids, linked leaves, root
children, separator keys, and row-id payloads in leaves. Point and range reads walk the leaf payloads.
Ordered entries are retained as the mutation staging structure so inserts and deletes can rebuild a
deterministic shallow layout until full incremental split/merge logic is warranted.

Next step: replace rebuild-on-write with incremental leaf/internal-page split and merge logic.

## Write-Ahead Log

The WAL records logical operations before mutating in-memory state:

- create database/table/index
- insert
- update
- delete
- drop/rename table
- save

The WAL persists append-only binary log records with a versioned header per record. Mutating
executor operations write replayable payloads. Startup recovery loads the latest saved snapshot,
then replays WAL records after the last save checkpoint. If no saved snapshot exists, recovery
replays the WAL from the beginning. Successful saves are written through a temporary snapshot file
and then checkpoint the WAL.

Next step: introduce physical redo records and crash-simulation tests for partial writes.

## Prepared Statements

Prepared statements keep named SQL templates with `?` placeholders. Execution binds literal values,
parses the resolved SQL, and routes it through the same planner and executor paths as direct SQL.
This keeps the first implementation compact while establishing a public API that can later move to
typed parameterized AST nodes.

Next step: store prepared ASTs with typed parameter slots instead of reparsing bound SQL.

## Joins

Joins support a single equi-join with projected or `SELECT *` output, qualified output column names,
joined `WHERE`, `ORDER BY`, and `LIMIT`. Execution builds an in-memory lookup over the right side
and scans the left side. Later planner work can choose between nested-loop and hash join variants
using table statistics.

Next step: add planner-selected join algorithms and support multiple joins.

## MVCC

The MVCC layer introduces transaction identifiers, transaction state management, and row-version
chains. `Table` records row versions during inserts, updates, deletes, and row replacement, and it
exposes transaction-aware snapshot APIs. The executor now routes active-transaction reads through
those APIs, while user-facing rollback still restores the snapshot copy captured at `BEGIN`.

Next step: replace snapshot-copy rollback with undo records or commit-aware MVCC visibility.

## Buffer Pool

The buffer pool is an LRU cache for fixed-size page payloads. `Table` delegates physical row storage
through a `RowStore` interface and defaults to `PageRowStore`, which groups rows into compact pages
and mirrors page bytes through the LRU buffer pool. `VectorRowStore` remains available for simple
in-memory storage tests.

Next step: make page payloads the source of truth by deserializing rows from buffer-pool pages.

## Query Planner

The first planner chooses between:

- full table scan
- hash index equality lookup
- ordered index range lookup

This gives the executor an explicit planning step without introducing a cost model that is more
complicated than the available statistics justify.

Next step: collect table/index statistics and use them for cost-based access-path selection.
