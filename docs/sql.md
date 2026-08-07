# SQL Subset

VertexDB intentionally supports a small SQL subset first.

## Implemented

```sql
CREATE DATABASE company;
CREATE TABLE Employees (id INT, name STRING, salary DOUBLE);
CREATE TABLE People (id INT, nickname STRING NULL);
DROP TABLE Employees;
RENAME TABLE Employees TO Staff;
LIST TABLES;
INSERT INTO Employees VALUES (1, "Alice", 120000.0);
INSERT INTO People VALUES (1, "Al"), (2, NULL);
SELECT * FROM Employees;
SELECT name FROM Employees WHERE salary > 100000.0 LIMIT 10;
SELECT name FROM Employees WHERE salary > 100000.0 OR name = "Alice";
SELECT name FROM Employees WHERE salary > 100000.0 ORDER BY salary DESC LIMIT 10;
SELECT * FROM Employees JOIN Departments ON dept_id = id LIMIT 10;
SELECT Employees.name, Departments.dept
FROM Employees JOIN Departments ON Employees.dept_id = Departments.id
WHERE Departments.dept > "A"
ORDER BY Employees.name DESC
LIMIT 5;
SELECT Employees.name, Offices.city
FROM Employees
JOIN Departments ON Employees.dept_id = Departments.id
JOIN Offices ON Departments.office_id = Offices.id;
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
SELECT name FROM (
  SELECT id, name, salary FROM Employees WHERE salary > 100000.0
) AS high WHERE id = 1;
WITH joined AS (SELECT * FROM Employees JOIN Departments ON dept_id = id)
SELECT Employees.name, Departments.dept FROM joined;
EXPLAIN SELECT name FROM Employees WHERE id = 1 AND salary > 100000.0;
EXPLAIN WITH high AS (SELECT id, name, salary FROM Employees WHERE salary > 100000.0)
SELECT name FROM high WHERE id = 1;
ANALYZE;
ANALYZE TABLE Employees;
UPDATE Employees SET salary = 150000.0 WHERE id = 1;
DELETE FROM Employees WHERE id = 5;
CREATE INDEX idx_salary ON Employees(salary);
CREATE INDEX idx_neg_salary ON Employees((-salary));
CREATE INDEX idx_id_plus ON Employees((id+1));
SELECT name FROM Employees WHERE (-salary) = -120000.0;
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
expression. Equality predicates can use hash index lookup. Less-than and greater-than predicates can
use ordered index range lookup when the filtered column (or matching expression) is indexed.
Compound `AND` predicates select the cheapest indexable access path using live row counts, index
distinct-key statistics, and optional `ANALYZE` histograms (equality ≈ \(N/D\), range ≈ histogram
selectivity or \(N/3\), `IN` ≈ histogram ndistinct or \(K\cdot N/D\)). When ≥2 equality (or
expression-equality) conjuncts are indexed and their estimated intersection is cheaper than a single
index + residual, the planner chooses a multi-index intersect of sorted `RowId` lists; `EXPLAIN`
lists the intersected columns. Remaining conjuncts evaluate as a residual filter. Top-level `OR`
of equality (or expression-equality) index probes uses a multi-index union of sorted `RowId` lists
when the indexable subset is cheaper than a full scan; `EXPLAIN` lists the unioned columns.
Non-indexable disjuncts become a residual OR complementary scan (partial OR, `residual: yes`).
When no disjunct is indexable, the planner keeps a full scan. An `OR` nested under
`AND` may remain as a residual while another conjunct uses an index.

`WITH` CTEs default to always-inline (same as `AS NOT MATERIALIZED`) so outer filters can use
base-table indexes. `AS MATERIALIZED` fences the CTE: the body is executed into an ephemeral table
(with hash indexes on projected columns), and the outer query plans against that temp — `EXPLAIN`
notes `materialized CTE <name>`. Derived tables `FROM (SELECT …) [AS] alias` normalize to the same
inline path. `WHERE col IN (SELECT …)` materializes uncorrelated subqueries (which themselves may
use indexes) and probes the outer column via hash index `IN` lookup when indexed. Correlated
`IN` / `EXISTS` with outer refs (`table.column` or unambiguous unqualified names) bind outer values
per candidate row for up to two outer FROM frames (main query plus one mid-level subquery). Deeper
correlation is rejected. CTE and derived-table bodies may include equi-joins (including left-deep
multi-join chains). One level of nested `WITH` inside a CTE body is supported; deeper nesting,
`WITH` inside `IN`/`EXISTS`, outer `JOIN` against a CTE/derived alias, and `JOIN` inside
`IN`/`EXISTS` subqueries are not.

`CREATE INDEX idx ON t(column)` builds maintained hash and ordered indexes on a column.
`CREATE INDEX idx ON t((expr))` builds the same structures on an evaluated expression key, where
`expr` is a column, unary `-column`, or `column +/- literal`. Predicates of the form `(expr) = const`
or `(expr) >/< const` can use the expression index; `EXPLAIN` reports expression hash/ordered
access. Expression metadata is stored with index definitions in snapshot v4 (`expr:…` encoding) so
SAVE/LOAD restores expression indexes without losing keys.

`ANALYZE` / `ANALYZE TABLE name` scans live rows and builds per-column equi-height histograms
(default 32 buckets) plus distinct counts. Histograms feed range/`IN` selectivity in the planner.
Histogram blobs are persisted in snapshot v4 after index pages (`VDBHIST1`); older v4 files without
the section load with empty stats until the next `ANALYZE`.

`EXPLAIN` runs the same rewrite and planning path as `SELECT` and returns a textual plan describing
the access path or each join algorithm in a left-deep chain, CTE inlining/materialization notes,
residual status, `est_rows` / `cost`, and an `aggregation` marker when aggregates or `GROUP BY` are
present.

`JOIN` supports left-deep equi-join chains (`t0 JOIN t1 ON … JOIN t2 ON …`). Joined result columns
are qualified as `LeftTable.column` and `RightTable.column`. Projection, `WHERE`, `ORDER BY`, and
`LIMIT` can reference qualified columns; unqualified references are allowed when the column name is
not ambiguous. The planner chooses between an in-memory hash join and a nested-loop index probe per
join when a join key is indexed and cheaper; after the first join, the left side is an intermediate
row set so only hash join or right-side index probe apply.

Aggregates `COUNT(*)`, `COUNT(col)`, `SUM`, `AVG`, `MIN`, and `MAX` run as a hash aggregate after
filter/join. `GROUP BY` is required for non-aggregated selected columns; `ORDER BY`/`LIMIT` apply to
group results. `SELECT *` is rejected with aggregates/`GROUP BY`.

`UPDATE` and `DELETE` evaluate their `WHERE` clause with a full scan of live rows. They do not yet
use the planner's index access paths (intentional v1 limitation).

Prepared statements parse once into a typed `Query` AST with `?` parameter slots (`Value` parameter
placeholders). `EXECUTE name VALUES (...)` binds parameters into a cloned AST and executes without
re-tokenizing or reparsing.

`SAVE DATABASE` and `LOAD DATABASE` use a versioned binary format (magic `TCRDB001`, current
page-payload + index-pages format v4, extension `.tcrdb`) under the executor's storage root. Current
snapshots store schemas, index definitions (column or `expr:`-prefixed expression metadata),
`rowsPerPage`, capacity, free-list order, serialized page-directory payloads, durable B+ tree /
hash index pages, and optional per-column histogram blobs so sparse IDs, page bytes, indexes, and
`ANALYZE` stats survive checkpoints without an index rebuild.
Older page-payload v3, sparse v2, and dense v1 snapshots remain readable (v1–v3 still rebuild indexes
after rows). `LOAD DATABASE` without a name reloads the active database when one exists, otherwise it
loads the first saved database file.
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
flushes deferred redo as one atomic WAL batch then marks the transaction committed and discards the
undo log; `ROLLBACK` applies the undo log LIFO on the same database instance and drops deferred WAL.
While a transaction is active, `CREATE DATABASE`, `CREATE TABLE`, `DROP TABLE`, `RENAME TABLE`,
`CREATE INDEX`, `SAVE DATABASE`, and `LOAD DATABASE` are rejected.

## Types

- `INT`: stored as signed 64-bit integer.
- `DOUBLE`: stored as C++ `double`.
- `STRING`: stored as `std::string`.
- `NULL`: allowed only for columns declared nullable with `NULL`.

## Near-Term Grammar Work

- Better diagnostics with source positions.
- Specialized indexes for substring/regex predicates.
- Correlation deeper than two outer frames / `WITH` inside subqueries.
- Outer / non-equi joins.
