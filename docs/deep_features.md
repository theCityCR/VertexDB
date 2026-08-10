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
descend from the root; range scans follow `nextLeaf` links. Snapshot format v4 persists B+ tree
pages via `exportPages` / `replaceFromPages` so SAVE/LOAD does not rebuild ordered indexes from rows.

## Write-Ahead Log

The WAL records durable operations before they must survive process restart:

- create database/table/index (logical SQL or name payloads)
- page-image DML redo (dirty heap pages + touched index pages; optional free-list/capacity metadata)
- drop/rename table
- save

The WAL persists append-only binary log records with a versioned header per record. Mutating
executor DML writes `PageImageRedo` payloads. Inside an open transaction, those records are buffered
in the executor and flushed on `COMMIT` as a single batch record (dropped on `ROLLBACK`) so a torn
commit cannot partially apply the transaction. Autocommit DML and DDL still append immediately.
Startup recovery loads the latest saved snapshot, then replays WAL records after the last save
checkpoint. `readAll` returns only complete records and ignores a truncated trailing write (crash
mid-append). If no saved snapshot exists, recovery replays the WAL from the beginning. Successful
saves are written through a temporary snapshot file and then checkpoint the WAL. Legacy
`PhysicalRedo` row after-images and logical `Insert`/`Update`/`Delete` SQL records remain replayable
for older WAL files.

## Prepared Statements

Prepared statements parse the template SQL once into a typed `Query` AST. `?` placeholders become
`Value` parameter slots. `EXECUTE` binds a `vector<Value>` into a cloned AST and runs the normal
planner/executor path without re-tokenizing or reparsing. `QueryExecutor::preparedAst` exposes the
stored AST for tests and introspection.

## Joins

Joins support left-deep `INNER` / `LEFT` / `RIGHT` / `FULL [OUTER] JOIN` and `CROSS JOIN` chains
with projected or `SELECT *` output, qualified output column names, joined `WHERE`, `ORDER BY`, and
`LIMIT`. Non-`CROSS` joins use `ON col op col` (`=`, `<`, or `>`). Equi-joins may use hash join
(build the right side) or nested-loop index probe when a join key is indexed and cheaper; non-equi
and outer joins use nested-loop compare with null-padding on unmatched preserved sides. After the
first join, the left side is an intermediate row set, so only hash join or right-side index probe
apply for remaining inner equi-joins. `EXPLAIN` reports each join algorithm and cost.

## Aggregates

`COUNT(*)`, `COUNT(col)`, `SUM`, `AVG`, `MIN`, and `MAX` are hash-aggregated after filter/join.
`GROUP BY` validates that non-aggregated selected columns are grouped; `ORDER BY`/`LIMIT` apply to
group output. `EXPLAIN` adds an `aggregation` marker.

## MVCC

The MVCC layer introduces SQL transaction identifiers, commit sequences, row-version chains, and
commit-aware snapshot reads. DML stamps `createdBy`/`deletedBy` with the active SQL transaction id
(or an immediately committed autocommit id). `BEGIN` captures a `ReadSnapshot` (`self` +
`maxCommitSeq`); SELECTs always evaluate visibility through that snapshot so readers see only
committed creators/deleters at or before the watermark, plus their own uncommitted writes. User-facing
rollback still applies a per-transaction undo log against the live database without cloning it.
Logical DML WAL records are replaced by page-image redo: buffered while a transaction is active,
flushed as one batch on `COMMIT`, and dropped on `ROLLBACK`. Legacy physical row-image redo remains
replayable.

## Buffer Pool

The buffer pool is an LRU cache of serialized page payloads, sized by page count (not fixed byte
width). `Table` delegates physical row storage through a `RowStore` interface and defaults to
`PageRowStore`, which groups rows into pages with a fixed number of row slots per page. Serialized
page bytes in an in-memory page directory are the source of truth; the buffer pool caches those
pages and fills on miss so reads deserialize live slots from page payloads. Both `PageRowStore` and
`VectorRowStore` keep stable row IDs with tombstones and LIFO free-list reuse: deletes leave holes,
and inserts reuse freed IDs before allocating new capacity. Database snapshots (format v4) persist
`rowsPerPage`, capacity, free-list order, serialized page-directory payloads, and index pages
(B+ tree nodes + hash buckets) so row IDs, page bytes, and indexes survive save/load without an
index rebuild. Legacy page-payload v3, sparse v2, and dense v1 snapshots remain readable.

## Query Planner

The planner chooses between:

- full table scan
- hash index equality lookup
- ordered index range lookup
- hash index `IN` multi-lookup
- ordered index prefix `LIKE` (`lit%`)
- multi-index equality or trigram intersect (sorted `RowId` intersection of ≥2 probes)

Costs use live table row counts and per-index distinct-key counts (`Table::indexDistinctCount`),
plus optional equi-height histograms from `ANALYZE` (`Table::columnHistogram`):

- equality ≈ \(N / D\) (average rows per key)
- range ≈ histogram bucket selectivity when present, otherwise \(N / 3\)
- `IN` ≈ histogram ndistinct when present, otherwise \(K \cdot (N / D)\)

When ≥2 indexable equality conjuncts exist, the planner estimates intersection under independence
(\(N \cdot \prod 1/D_i\)) and chooses `Intersect` when that cost beats the best single
index path. Tie-breaks still prefer any index over a full scan and equality over range/`IN` when
costs match. For non-intersect `AND` plans the cheapest indexable conjunct drives the access path;
remaining conjuncts become a residual filter. Top-level `OR` of equality index probes estimates
union under independence (\(N \cdot (1 - \prod(1 - 1/D_i))\)) over the indexable subset and chooses
`Union` when that cost beats a full scan. Non-indexable disjuncts become a residual OR
evaluated as a complementary scan after the index union (partial OR); `EXPLAIN` reports
`residual: yes` and a residual-OR note. When no arm is indexable (or the indexable union is not
cheaper), the planner keeps a full scan. An `OR` nested under `AND` may stay as a residual while
another conjunct uses an index. `EXPLAIN` surfaces the chosen path (including intersected or
unioned columns), residual status, `est_rows` / `cost`, and rewrite notes such as CTE inlining.

Equi-joins are planned with the same statistics: hash join versus nested-loop index probe, including
per-join planning for left-deep multi-join chains. Non-equi and `LEFT` joins fall back to
nested-loop compare.

A rewriter inlines `WITH` CTEs by default (`AS NOT MATERIALIZED` is explicit) and derived tables
(`FROM (SELECT …) [AS] alias`, normalized to synthetic CTEs) into the outer `SELECT`.
`AS MATERIALIZED` fences the CTE into an ephemeral table before planning. Outer `JOIN` against a
CTE/derived alias also force-materializes so body filters stay scoped inside the temp.
Uncorrelated `IN (SELECT …)` / `EXISTS (SELECT …)` subqueries (optionally headed by `WITH`, and
optionally containing joins) materialize into value lists when uncorrelated; correlated
`IN`/`EXISTS` bind outer scopes per row for up to four FROM frames, including `FROM` / `JOIN`
table aliases. Nested `WITH` up to depth 3 reuses the same inliner. Minimal `WITH RECURSIVE`
(`UNION ALL`, delta binding, iteration/row caps) always force-materializes. Caps default to 1000
iterations and 100000 accumulated rows (`recursiveCteLimits()`); the row cap is checked before
inserting a recursive step.
Expression indexes match `(expr) =/>/< const` predicates; `trigram(column)` indexes serve substring
`LIKE '%lit%'`. Regex `~` remains a residual full scan. CTE/derived bodies may carry left-deep
`INNER`/`LEFT`/`RIGHT`/`FULL`/`CROSS` joins (including multi-join chains) through inlining or
materialization. Aggregates/`GROUP BY` are planned as a post-join hash aggregate (`EXPLAIN`
reports `aggregation`).

### CTE index demo

```sql
WITH high AS (
  SELECT id, name, salary FROM Employees WHERE salary > 100000.0
)
SELECT name FROM high WHERE id = 1;
```

`EXPLAIN` chooses hash index equality on `id`, keeps `salary > …` as a residual, and notes
`inlined CTE high`. `WITH … AS MATERIALIZED` instead notes `materialized CTE high` and plans
against the temp result (fencing the base-table `id` index). Full write-up, limitations, and
comparison artifacts: [cte_index_wedge.md](cte_index_wedge.md) (Demo) and
[cte_materialize_comparison.md](cte_materialize_comparison.md).
