# SQL Subset

VertexDB intentionally supports a small SQL subset first.

## Implemented

```sql
CREATE DATABASE company;
DROP DATABASE company;
CREATE TABLE Employees (id INT, name STRING, salary DOUBLE);
CREATE TABLE People (id INT NOT NULL, nickname STRING NULL);
CREATE TABLE Accounts (id INT PRIMARY KEY, email STRING UNIQUE);
CREATE TABLE Pay (id INT PRIMARY KEY, salary DOUBLE CHECK (salary > 0.0));
CREATE TABLE Orders (id INT PRIMARY KEY, customer_id INT REFERENCES Customers(id));
DROP TABLE Employees;
RENAME TABLE Employees TO Staff;
LIST TABLES;
INSERT INTO Employees VALUES (1, "Alice", 120000.0);
INSERT INTO People VALUES (1, "Al"), (2, NULL);
SELECT * FROM Employees;
SELECT name FROM Employees WHERE salary > 100000.0 LIMIT 10;
SELECT name FROM Employees WHERE salary > 100000.0 OR name = "Alice";
SELECT name FROM Employees WHERE id IN (1, 3);
SELECT name FROM Employees WHERE salary > 100000.0 ORDER BY salary DESC LIMIT 10;
SELECT * FROM Employees JOIN Departments ON dept_id = id LIMIT 10;
SELECT Employees.name, Departments.dept
FROM Employees LEFT JOIN Departments ON Employees.dept_id = Departments.id
WHERE Departments.dept > "A"
ORDER BY Employees.name DESC
LIMIT 5;
SELECT Employees.name, Offices.city
FROM Employees
JOIN Departments ON Employees.dept_id = Departments.id
JOIN Offices ON Departments.office_id = Offices.id;
SELECT e.name, d.dept FROM Employees e
LEFT OUTER JOIN Departments d ON e.dept_id > d.id;
SELECT e.name, d.dept FROM Employees e
RIGHT JOIN Departments d ON e.dept_id = d.id;
SELECT e.name, d.dept FROM Employees e
FULL OUTER JOIN Departments d ON e.dept_id = d.id;
SELECT e.name, d.dept FROM Employees e CROSS JOIN Departments d;
SELECT name FROM Employees WHERE name LIKE "Al%";
SELECT name FROM Employees WHERE note LIKE "%hello%";
SELECT name FROM Employees WHERE name ~ "^A.*";
SELECT dept_id, COUNT(*), SUM(salary), AVG(salary), MIN(salary), MAX(salary)
FROM Employees
GROUP BY dept_id
ORDER BY dept_id;
SELECT COUNT(*) FROM Employees;
SELECT COUNT(name), MIN(salary), MAX(salary) FROM Employees;
WITH high AS (SELECT id, name, salary FROM Employees WHERE salary > 100000.0)
SELECT name FROM high WHERE id = 1;
WITH high AS MATERIALIZED (SELECT id, name, salary FROM Employees WHERE salary > 100000.0)
SELECT name FROM high WHERE id = 1;
WITH high AS NOT MATERIALIZED (SELECT id, name, salary FROM Employees WHERE salary > 100000.0)
SELECT name FROM high WHERE id = 1;
SELECT name FROM Employees WHERE id IN (SELECT id FROM Employees WHERE salary > 100000.0);
SELECT name FROM Employees WHERE EXISTS (SELECT emp_id FROM Bonuses WHERE emp_id = Employees.id);
SELECT name FROM Employees WHERE id IN (SELECT emp_id FROM Bonuses WHERE emp_id = id);
SELECT name FROM Employees WHERE id IN (
  SELECT Employees.id FROM Employees JOIN Departments ON Employees.dept_id = Departments.id);
SELECT name FROM (
  SELECT id, name, salary FROM Employees WHERE salary > 100000.0
) AS high WHERE id = 1;
WITH high AS (SELECT id, name, dept_id FROM Employees WHERE id = 1)
SELECT high.name, Departments.dept FROM high JOIN Departments ON high.dept_id = Departments.id;
WITH joined AS (SELECT * FROM Employees JOIN Departments ON dept_id = id)
SELECT Employees.name, Departments.dept FROM joined;
WITH RECURSIVE tree AS (
  SELECT id, parent_id, name FROM Nodes WHERE id = 1
  UNION ALL
  SELECT Nodes.id, Nodes.parent_id, Nodes.name FROM Nodes JOIN tree
  ON Nodes.parent_id = tree.id
)
SELECT name FROM tree;
SELECT id FROM A UNION SELECT id FROM B ORDER BY id;
SELECT id FROM A INTERSECT SELECT id FROM B;
SELECT id FROM A EXCEPT SELECT id FROM B;
EXPLAIN SELECT name FROM Employees WHERE id = 1 AND salary > 100000.0;
EXPLAIN ANALYZE SELECT name FROM Employees WHERE id = 1 AND salary > 100000.0;
EXPLAIN UPDATE Employees SET name = "x" WHERE id = 1;
EXPLAIN DELETE FROM Employees WHERE id = 2;
EXPLAIN INSERT INTO Employees VALUES (3, "Cara", 110000.0);
EXPLAIN WITH high AS (SELECT id, name, salary FROM Employees WHERE salary > 100000.0)
SELECT name FROM high WHERE id = 1;
ANALYZE;
ANALYZE TABLE Employees;
UPDATE Employees SET salary = 150000.0 WHERE id = 1;
DELETE FROM Employees WHERE id = 5;
CREATE INDEX idx_salary ON Employees(salary);
CREATE INDEX idx_neg_salary ON Employees((-salary));
CREATE INDEX idx_id_plus ON Employees((id+1));
CREATE INDEX idx_note_tri ON Employees((trigram(note)));
DROP INDEX idx_salary ON Employees;
SELECT name FROM Employees WHERE (-salary) = -120000.0;
WITH mid AS (
  WITH inner AS (SELECT id, name FROM Employees WHERE salary > 100000.0)
  SELECT * FROM inner
)
SELECT name FROM mid WHERE id = 1;
PREPARE by_id AS "SELECT name FROM Employees WHERE id = ?;";
EXECUTE by_id VALUES (1);
SAVE DATABASE;
LOAD DATABASE;
LOAD DATABASE company;
BEGIN;
COMMIT;
ROLLBACK;
EXIT;
```

`CREATE INDEX` builds maintained hash and ordered index structures for the target column or
expression. `DROP INDEX name ON table` removes a named index from that table (index names are
per-table). Equality predicates can use hash index lookup. Less-than and greater-than predicates can
use ordered index range lookup when the filtered column (or matching expression) is indexed.
Prefix `LIKE 'lit%'` (no other wildcards) can use an ordered index prefix scan; substring
`LIKE '%lit%'` can use a trigram expression index (`CREATE INDEX … ON t((trigram(col)))`) via
multi-key intersect, with the `LIKE` kept as a residual. Regex `col ~ 'pattern'` is always a
residual full-scan filter. Compound `AND` predicates select the cheapest indexable access path using
live row counts, index distinct-key statistics, and optional `ANALYZE` histograms (equality ≈
\(N/D\), range ≈ histogram selectivity or \(N/3\), `IN` ≈ histogram ndistinct or \(K\cdot N/D\)).
When ≥2 equality (or expression-equality) conjuncts are indexed and their estimated intersection is
cheaper than a single index + residual, the planner chooses a multi-index intersect of sorted
`RowId` lists; `EXPLAIN` lists the intersected columns. Remaining conjuncts evaluate as a residual
filter. Top-level `OR` of equality (or expression-equality) index probes uses a multi-index union of
sorted `RowId` lists when the indexable subset is cheaper than a full scan; `EXPLAIN` lists the
unioned columns. Non-indexable disjuncts become a residual OR complementary scan (partial OR,
`residual: yes`). When no disjunct is indexable, the planner keeps a full scan. Same-column
equality `OR` (top-level or nested under `AND`) is rewritten to an `IN` list so `HashIn` can win.
A heterogeneous `OR` nested under `AND` whose equality-indexable arms are non-empty becomes a
Union child of a multi-index Intersect (composite Intersect∪Union) when cheaper than the best
single conjunct; `EXPLAIN` reports e.g. `multi-index intersect on id with union(dept, city)`.
Non-indexable nested-OR arms become a complementary residual under the outer AND (partial nested
OR, `residual: yes`). If no nested-OR arm is equality-indexable, the whole `OrPred` stays an AND
residual.

`WITH` CTEs default to always-inline (same as `AS NOT MATERIALIZED`) so outer filters can use
base-table indexes. `AS MATERIALIZED` fences the CTE: the body is executed into an ephemeral table
(with hash indexes on projected columns), and the outer query plans against that temp — `EXPLAIN`
notes `materialized CTE <name>`. Derived tables `FROM (SELECT …) [AS] alias` normalize to the same
inline path. `WHERE col IN (v1, v2, …)` builds an `InListPred` (HashIn when indexed).
`WHERE col IN (SELECT …)` materializes uncorrelated subqueries (which themselves may
use indexes) and probes the outer column via hash index `IN` lookup when indexed. Correlated
`IN` / `EXISTS` with outer refs (`table.column`, `alias.column`, or unambiguous unqualified names)
bind outer values per candidate row for up to eight outer FROM frames. Deeper correlation is
rejected. `FROM` / `JOIN` tables accept an optional `[AS] alias` used as the qualification and
correlation scope (aliases rewrite to physical table qualifiers on join results). CTE and
derived-table bodies may include left-deep joins (`INNER` / `LEFT` / `RIGHT` / `FULL` / `CROSS`).
`WITH` nesting depth up to 6 is supported (nested `WITH` up to six levels inside a CTE body;
seven `WITH` frames in a chain).
`WITH` / derived tables and `JOIN` are allowed inside `IN`/`EXISTS` subqueries. Outer `JOIN`
against a CTE/derived alias force-materializes the CTE (body filters stay inside the temp).

`WITH RECURSIVE name AS [ACCUMULATOR] ( anchor UNION [ALL] recursive_arm )` materializes a working
table by evaluating the anchor, then repeatedly evaluating the recursive arm. By default the CTE
name is bound to the previous iteration's **delta** (new rows only). `AS ACCUMULATOR` binds the
name to the full working table instead. Each recursive arm must reference exactly one recursive CTE
name from the same `WITH` list exactly once (as a `FROM`/`JOIN` table) — usually itself (linear
recursion), or another recursive CTE in a mutual group. Bare `UNION` (deduplicating) excludes rows
already present in the accumulating working table, which stops graph cycles. `UNION ALL` keeps
duplicates and can diverge; with `AS ACCUMULATOR`, a naïve `UNION ALL` hierarchy walk typically
does not shrink the delta and hits the iteration cap. `WITH RECURSIVE` may declare multiple
recursive CTEs in one `WITH` list: independent CTEs evaluate separately; mutually recursive CTEs
(a cycle of arm references) co-evaluate in lockstep. Iteration stops when every delta in the group
is empty, or when a safety cap is hit (1000 iterations or 100000 accumulated rows) — those caps are
intentional v1 limits. The row cap is checked before inserting a recursive step so a single
oversized step cannot partially accumulate past the limit.

Top-level and CTE-body set operations are left-associative: `UNION` / `UNION ALL` / `INTERSECT` /
`INTERSECT ALL` / `EXCEPT` / `EXCEPT ALL`. Distinct forms deduplicate; `ALL` forms use multiset
semantics (`INTERSECT ALL` keeps `min` multiplicity, `EXCEPT ALL` keeps leftover left copies).
Arms must project the same column count; `ORDER BY` / `LIMIT` after the chain apply to the combined
result. Set-op CTE bodies are force-materialized (they are not inlined).

`CREATE INDEX idx ON t(column)` builds maintained hash and ordered indexes on a column.
`CREATE INDEX idx ON t((expr))` builds index structures on an evaluated expression key, where
`expr` is a column, unary `-column`, `column +/- literal`, or `trigram(column)` (hash-only trigram
keys for substring `LIKE`). Predicates of the form `(expr) = const` or `(expr) >/< const` can use
arithmetic expression indexes; `EXPLAIN` reports expression hash/ordered access. Expression
metadata is stored with index definitions in snapshot v4+ (`expr:…` encoding) so SAVE/LOAD restores
expression indexes without losing keys. `DROP INDEX idx ON t` removes the named index; reserved
constraint indexes (`__pk_*` / `__uq_*`) cannot be dropped.

Column constraints on `CREATE TABLE`:

- Columns are `NOT NULL` by default. Write `col TYPE NULL` to allow nulls, or `col TYPE NOT NULL`
  to state the default explicitly. Assigning `NULL` on insert/update throws
  `NOT NULL constraint violation on column <name>` — an ACID **Consistency** guarantee, not only a
  storage shape check.
- `col TYPE PRIMARY KEY` — implies `NOT NULL` and uniqueness; at most one primary-key column per
  table (multi-column keys are out of scope until composite indexes exist). Rejects `NULL PRIMARY KEY`.
- `col TYPE UNIQUE` — rejects duplicate non-NULL values on `INSERT`/`UPDATE`; multiple `NULL`s are
  allowed when the column is also `NULL`.
- `CHECK (predicate)` — column-level or table-level. Predicate is simple column comparisons
  (`=`, `<`, `>`) combined with `AND` / `OR` (no subqueries, `IN`, `LIKE`, or regex). Evaluated on
  the full row image at `INSERT`/`UPDATE`. Rejects only when the predicate is FALSE; NULL comparisons
  are UNKNOWN and accepted (SQL CHECK semantics). Error:
  `CHECK constraint violation: <predicate>`.
- `REFERENCES Parent(col)` or `FOREIGN KEY (col) REFERENCES Parent(col)` — single-column foreign
  keys. Parent column must be `PRIMARY KEY` or `UNIQUE` and already exist (self-FK allowed on the
  table being created). Optional `ON DELETE NO ACTION` / `ON UPDATE NO ACTION` (the only supported
  actions). NULL child keys are accepted (MATCH SIMPLE). Parent existence uses the writer's
  SI-visible snapshot. Deleting or updating a referenced parent key is rejected while visible
  children still reference it. `DROP TABLE` / `RENAME TABLE` of a referenced parent is rejected.
  Errors: `FOREIGN KEY constraint violation on column …` /
  `FOREIGN KEY constraint violation: key is still referenced`.
- VertexDB auto-creates a maintained column index (`__pk_<col>` / `__uq_<col>`) when no column index
  already exists, so equality predicates on those columns use `HashEq` without a manual
  `CREATE INDEX`.

`ANALYZE` / `ANALYZE TABLE name` scans live rows and builds per-column equi-height histograms
(default 32 buckets) plus distinct counts. Histograms feed range/`IN` selectivity in the planner.
Histogram blobs are persisted in snapshot v4+ after index pages (`VDBHIST1`); older v4 files without
the section load with empty stats until the next `ANALYZE`.

`EXPLAIN` runs the same rewrite and planning path as `SELECT` and returns a textual plan describing
the access path or each join algorithm in a left-deep chain, CTE inlining/materialization notes,
residual status, `est_rows` / `cost`, and an `aggregation` marker when aggregates or `GROUP BY` are
present. `EXPLAIN UPDATE` / `EXPLAIN DELETE` plan the mutation `WHERE` with the same access-path
machinery (prefix `update:` / `delete:`) and do not write rows. `EXPLAIN INSERT` reports
`insert: N row(s) into <table>` after validating the target table exists, and does not write rows.
`EXPLAIN ANALYZE` remains SELECT/WITH-only (mutations including `INSERT` are rejected at parse).

`EXPLAIN ANALYZE SELECT …` / `EXPLAIN ANALYZE WITH … SELECT …` uses a **single pass**: plan as usual,
then execute the query once (without returning data rows) and append measured fields next to the
estimates:

| Field | Meaning |
| --- | --- |
| `actual_rows` | Rows after access path + residual / post-join `WHERE`, before `ORDER BY` / `LIMIT`; with aggregates/`GROUP BY`, row count after aggregation |
| `candidates` | Pre-residual index candidate count when a residual filter ran (omitted otherwise) |
| `actual_time_ms` | Wall time for the whole ANALYZE execute (on the first plan row) |

Plain `EXPLAIN` does not include these fields. `EXPLAIN ANALYZE` remains SELECT/WITH-only
(mutations are rejected at parse). This is distinct from standalone `ANALYZE` / `ANALYZE TABLE`,
which builds planner histograms.

`JOIN` / `INNER JOIN`, `LEFT` / `RIGHT` / `FULL [OUTER] JOIN`, and `CROSS JOIN` support left-deep
chains (`t0 [AS a] JOIN t1 [AS b] ON … JOIN t2 ON …`). Non-`CROSS` joins use `ON col op col` where
`op` is `=`, `<`, or `>`; `CROSS JOIN` has no `ON`. Equi-joins may use hash join or nested-loop
index probe; non-equi and outer joins (`LEFT` / `RIGHT` / `FULL`) use nested-loop compare (no hash
join), with null-padding on unmatched preserved sides. Joined result columns are qualified with
physical table names (`LeftTable.column` / `RightTable.column`); `FROM`/`JOIN` aliases in
`SELECT`/`WHERE`/`ON` rewrite to those qualifiers. Projection, `WHERE`, `ORDER BY`, and `LIMIT` can
reference qualified columns (alias or table); unqualified references are allowed when the column
name is not ambiguous. After the first join, the left side is an intermediate row set so only hash
join or right-side index probe apply for remaining inner equi-joins.

Aggregates `COUNT(*)`, `COUNT(col)`, `SUM`, `AVG`, `MIN`, and `MAX` run as a hash aggregate after
filter/join. `GROUP BY` is required for non-aggregated selected columns; `ORDER BY`/`LIMIT` apply to
group results. `SELECT *` is rejected with aggregates/`GROUP BY`.

`UPDATE` and `DELETE` plan their `WHERE` clause with the same cost-based access paths as
`SELECT` (hash equality, ordered range, `IN`, intersect/union, prefix `LIKE`, residuals). Candidate
`RowId`s are collected first, then mutated, so mid-statement index rebuilds cannot skip hits.
`EXPLAIN UPDATE` / `EXPLAIN DELETE` surface that access path without writing; `EXPLAIN ANALYZE` remains
SELECT/WITH-only.

Prepared statements parse once into a typed `Query` AST with `?` parameter slots (`Value` parameter
placeholders). `EXECUTE name VALUES (...)` binds parameters into a cloned AST and executes without
re-tokenizing or reparsing.

`SAVE DATABASE` and `LOAD DATABASE` use a versioned binary format (magic `TCRDB001`, current
page-payload + index-pages + column constraint flags + CHECK + FOREIGN KEY format v7, extension
`.tcrdb`) under the
executor's storage root. `SAVE` writes a temporary snapshot, durable-syncs that file (flush+fsync /
`F_FULLFSYNC` on macOS when available; `FlushFileBuffers` on Windows), renames it into place, then
durable-syncs the storage directory on POSIX so the directory entry survives power loss — the same
discipline as WAL `COMMIT`. Current snapshots store schemas (including `UNIQUE` / `PRIMARY KEY`
flags, `CHECK` predicate text, and `FOREIGN KEY` metadata), index definitions (column or
`expr:`-prefixed expression metadata),
`rowsPerPage`, capacity,
free-list order, serialized page-directory payloads, durable B+ tree / hash index pages, and
optional per-column histogram blobs so sparse IDs, page bytes, indexes, constraints, and `ANALYZE`
stats survive checkpoints without an index rebuild.
Older CHECK v6, constraint-flags v5, index-pages v4, page-payload v3, sparse v2, and dense v1
snapshots remain
readable (v1–v3 still
rebuild indexes after rows). `LOAD DATABASE` without a name reloads the active database when one
exists, otherwise it loads the first saved database file.
Query executors also recover automatically on startup by loading the latest saved snapshot and
replaying WAL records after that checkpoint. DML redo uses page images (`PageImageRedo`); DDL uses
logical SQL. Legacy `PhysicalRedo` row after-images remain replayable. Incomplete trailing WAL
records from a crash mid-append are ignored. If no snapshot exists, startup recovery replays the WAL
from the beginning. Successful saves checkpoint the WAL so future recovery only replays post-save
changes.

Transactions use transaction state tracking, commit-aware MVCC snapshot reads, and undo-log rollback
for DML. `BEGIN` opens a transaction, captures a commit-seq read snapshot, and clears the undo log
and any deferred WAL buffer; SELECTs use that snapshot (plus read-your-writes) so concurrent
uncommitted or post-`BEGIN` commits stay invisible; mutating `INSERT`/`UPDATE`/`DELETE` stamp SQL
transaction ids, append compensating undo records, and buffer page-image redo records; `COMMIT`
flushes deferred redo as one atomic WAL batch (each append flush+fsyncs the WAL file) then marks
the transaction committed and discards the undo log; `ROLLBACK` applies the undo log LIFO on the
same database instance and drops deferred WAL.
Snapshot isolation **prevents** dirty reads, non-repeatable reads, and mid-txn phantoms under a held
snapshot. SSI **aborts** a later `COMMIT` on overlapping row read/write sets (write skew /
write–write) or on insert-phantom conflicts (predicate SIREAD summaries from SELECT/UPDATE/DELETE
scans vs inserted or update-produced row images). Column comparisons, `IN` lists, OR of those
column leaves, and column `LIKE` record real predicates; regex / subquery / expression-index probes
use conservative relation-membership SIREADs. There are no Postgres-style next-key locks. One
executor holds at most one open transaction; writers are serialized by the executor `LockManager`.
See [si_anomaly_wedge.md](si_anomaly_wedge.md).
While a transaction is active, `CREATE DATABASE`, `CREATE TABLE`, `DROP TABLE`, `RENAME TABLE`,
`CREATE INDEX`, and `DROP INDEX` are allowed: each applies immediately and pushes an undo record
(with deferred logical WAL until `COMMIT`; dropped on `ROLLBACK`). `DROP DATABASE` is rejected
while a transaction is active. `RENAME TABLE` remounts the same
table object and rewrites open-txn undo/pending WAL table names. `CREATE DATABASE` swaps in a new
empty database and restores the prior instance on rollback. `DROP DATABASE name` requires `name` to
be the active database, clears the in-memory instance, deletes the on-disk `.tcrdb` when present,
and appends a WAL record. `SAVE DATABASE` inside a transaction
implicitly `COMMIT`s (flush deferred WAL) then checkpoints. `LOAD DATABASE` inside a transaction
implicitly `ROLLBACK`s then replaces the in-memory database from the snapshot.

## Types

- `INT`: stored as signed 64-bit integer.
- `DOUBLE`: stored as C++ `double`.
- `STRING`: stored as `std::string`.
- `NULL`: allowed only for columns declared nullable with `NULL` (and never for `PRIMARY KEY` or
  default/`NOT NULL` columns). Insert/update of NULL into a non-nullable column is a
  `NOT NULL` constraint violation. Equality comparisons, equi-join keys, correlated `IN`/`EXISTS`
  outer binds, and `GROUP BY` keys treat `NULL = NULL` as true (VertexDB `Value` equality), unlike
  SQL's UNKNOWN. `LIKE` and `~` on a NULL column value do not match. `UNIQUE` columns allow multiple
  NULLs.

Tokenizer and core parser failures throw `ParseError` with 1-based `line`/`column` (message prefix
`line L, column C: …`). The CLI prints `error: ` plus that message.

## Remaining Grammar Gaps

Intentional v1 limits (documented, not near-term polish):

- Derived-table syntax in the JOIN position (`JOIN (SELECT …) AS alias`); use a CTE name or put
  the join inside the CTE/derived body instead
- Joins beyond left-deep chains with `ON col op col` (or no `ON` for `CROSS`)
