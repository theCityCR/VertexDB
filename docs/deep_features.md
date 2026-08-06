# Deep Features

## B+ Tree Index

The B+ tree exposes an ordered index API:

- point lookup
- less-than range lookup
- greater-than range lookup
- ordered row id storage per key

Internally it maintains explicit B+ tree layout metadata: leaf page ids, linked leaves, internal
children, separator keys, and row-id payloads in leaves. Inserts and deletes split and merge leaf
and internal nodes incrementally (fanout defaults to 64; capacity must be at least 2). Point lookups
descend from the root; range scans follow `nextLeaf` links.

Next step: persist index pages with the on-disk snapshot format (indexes are still rebuilt from row
data on load).

## Write-Ahead Log

The WAL records durable operations before they must survive process restart:

- create database/table/index (logical SQL or name payloads)
- physical DML redo (row after-images / erases at explicit row ids)
- drop/rename table
- save

The WAL persists append-only binary log records with a versioned header per record. Mutating
executor DML writes `PhysicalRedo` payloads (typed row images, not SQL text). Inside an open
transaction, those records are buffered in the executor and flushed on `COMMIT` as a single batch
record (dropped on `ROLLBACK`) so a torn commit cannot partially apply the transaction. Autocommit
DML and DDL still append immediately. Startup recovery loads the latest saved snapshot, then
replays WAL records after the last save checkpoint. `readAll` returns only complete records and
ignores a truncated trailing write (crash mid-append). If no saved snapshot exists, recovery
replays the WAL from the beginning. Successful saves are written through a temporary snapshot file
and then checkpoint the WAL. Legacy logical `Insert`/`Update`/`Delete` SQL records remain
replayable for older WAL files.

Next step: persist page payloads as the snapshot format and evolve redo toward page images.

## Prepared Statements

Prepared statements keep named SQL templates with `?` placeholders. Execution binds literal values,
parses the resolved SQL, and routes it through the same planner and executor paths as direct SQL.
This keeps the first implementation compact while establishing a public API that can later move to
typed parameterized AST nodes.

Next step: store prepared ASTs with typed parameter slots instead of reparsing bound SQL.

## Joins

Joins support a single equi-join with projected or `SELECT *` output, qualified output column names,
joined `WHERE`, `ORDER BY`, and `LIMIT`. The planner chooses between an in-memory hash join (build
the right side) and a nested-loop index probe when a join key is indexed and the estimated probe
cost is cheaper. `EXPLAIN` reports the chosen algorithm and cost.

Next step: support multiple joins and additional join strategies.

## MVCC

The MVCC layer introduces SQL transaction identifiers, commit sequences, row-version chains, and
commit-aware snapshot reads. DML stamps `createdBy`/`deletedBy` with the active SQL transaction id
(or an immediately committed autocommit id). `BEGIN` captures a `ReadSnapshot` (`self` +
`maxCommitSeq`); SELECTs always evaluate visibility through that snapshot so readers see only
committed creators/deleters at or before the watermark, plus their own uncommitted writes. User-facing
rollback still applies a per-transaction undo log against the live database without cloning it.
Logical DML WAL records are replaced by physical row-image redo: buffered while a transaction is
active, flushed as one batch on `COMMIT`, and dropped on `ROLLBACK`.

Next step: persist page payloads as the on-disk snapshot format and move redo toward page images.

## Buffer Pool

The buffer pool is an LRU cache of serialized page payloads, sized by page count (not fixed byte
width). `Table` delegates physical row storage through a `RowStore` interface and defaults to
`PageRowStore`, which groups rows into pages with a fixed number of row slots per page. Serialized
page bytes in an in-memory page directory are the source of truth; the buffer pool caches those
pages and fills on miss so reads deserialize live slots from page payloads. Both `PageRowStore` and
`VectorRowStore` keep stable row IDs with tombstones and LIFO free-list reuse: deletes leave holes,
and inserts reuse freed IDs before allocating new capacity. Database snapshots persist that sparse
layout so row IDs survive save/load.

Next step: persist page payloads as the on-disk snapshot format instead of typed sparse row
entries.

## Query Planner

The planner chooses between:

- full table scan
- hash index equality lookup
- ordered index range lookup
- hash index `IN` multi-lookup

Costs use live table row counts and per-index distinct-key counts (`Table::indexDistinctCount`):

- equality ≈ \(N / D\) (average rows per key)
- range ≈ \(N / 3\) (no histograms yet)
- `IN` ≈ \(K \cdot (N / D)\)

Tie-breaks still prefer any index over a full scan and equality over range/`IN` when costs match.
For `AND` predicates the cheapest indexable conjunct drives the access path; remaining conjuncts
become a residual filter. Top-level `OR` predicates remain full scans; an `OR` nested under `AND`
may stay as a residual while another conjunct uses an index. `EXPLAIN` surfaces the chosen path,
residual status, `est_rows` / `cost`, and rewrite notes such as CTE inlining.

Equi-joins are planned with the same statistics: hash join versus nested-loop index probe.

A rewriter always inlines `WITH` CTEs into the outer `SELECT` and materializes `IN (SELECT …)`
subqueries into value lists before planning, so nested SQL can still use base-table indexes.

### CTE index demo

```sql
WITH high AS (
  SELECT id, name, salary FROM Employees WHERE salary > 100000.0
)
SELECT name FROM high WHERE id = 1;
```

`EXPLAIN` chooses hash index equality on `id`, keeps `salary > …` as a residual, and notes
`inlined CTE high`. Materializing the CTE would build the high-salary set first and lose the
base-table `id` index for the outer filter. Full write-up, limitations, and comparison artifacts:
[cte_index_wedge.md](cte_index_wedge.md) (Demo) and
[cte_materialize_comparison.md](cte_materialize_comparison.md).

Next step: histograms / `ANALYZE` and multi-index AND optimization.
