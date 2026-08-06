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
WITH high AS (SELECT id, name, salary FROM Employees WHERE salary > 100000.0)
SELECT name FROM high WHERE id = 1;
SELECT name FROM Employees WHERE id IN (SELECT id FROM Employees WHERE salary > 100000.0);
EXPLAIN SELECT name FROM Employees WHERE id = 1 AND salary > 100000.0;
EXPLAIN WITH high AS (SELECT id, name, salary FROM Employees WHERE salary > 100000.0)
SELECT name FROM high WHERE id = 1;
UPDATE Employees SET salary = 150000.0 WHERE id = 1;
DELETE FROM Employees WHERE id = 5;
CREATE INDEX idx_salary ON Employees(salary);
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

`CREATE INDEX` builds maintained hash and ordered index structures for the target column. Equality
predicates can use hash index lookup. Less-than and greater-than predicates can use ordered index
range lookup when the filtered column is indexed. Compound `AND` predicates select the cheapest
indexable conjunct for an access path using live row counts and index distinct-key statistics
(equality ≈ \(N/D\), range ≈ \(N/3\), `IN` ≈ \(K\cdot N/D\)) and evaluate the remaining conjuncts as
a residual filter. Top-level `OR` predicates still force a full scan; an `OR` nested under `AND`
may remain as a residual while another conjunct uses an index.

`WITH` CTEs are always inlined into the outer `SELECT` (no materialization fence), so outer filters
can use base-table indexes. `WHERE col IN (SELECT …)` materializes the subquery (which itself may
use indexes) and then probes the outer column via hash index `IN` lookup when that column is
indexed. CTE bodies and `IN` subqueries are single-table in this version (no nested `WITH`, no
`JOIN` inside them). Correlated subqueries are not supported.

`EXPLAIN` runs the same rewrite and planning path as `SELECT` and returns a textual plan describing
the access path or join algorithm, CTE inlining notes, residual status, and `est_rows` / `cost`.

`JOIN` supports a single equi-join. Joined result columns are qualified as `LeftTable.column` and
`RightTable.column`. Projection, `WHERE`, `ORDER BY`, and `LIMIT` can reference qualified columns;
unqualified references are allowed when the column name is not ambiguous. The planner chooses
between an in-memory hash join and a nested-loop index probe when a join key is indexed and cheaper.

`UPDATE` and `DELETE` evaluate their `WHERE` clause with a full scan of live rows. They do not yet
use the planner's index access paths (intentional v1 limitation).

Prepared statements store a SQL string containing `?` placeholders. `EXECUTE name VALUES (...)`
binds values positionally, reparses the bound statement, and executes it through the normal engine.

`SAVE DATABASE` and `LOAD DATABASE` use a versioned binary format (magic `TCRDB001`, current sparse
format v2, extension `.tcrdb`) under the executor's storage root. Current snapshots store schemas,
indexes, capacity, free-list order, and live `(rowId, row)` entries so sparse IDs survive
checkpoints; older dense v1 snapshots remain readable. On load, indexes are registered before rows
are reloaded so index rebuilds populate from the restored row set. `LOAD DATABASE` without a name
reloads the active database when one exists, otherwise it loads the first saved database file.
Query executors also recover automatically on startup by loading the latest saved snapshot and
replaying WAL records after that checkpoint. DML redo uses physical row after-images; DDL uses
logical SQL. Incomplete trailing WAL records from a crash mid-append are ignored. If no snapshot
exists, startup recovery replays the WAL from the beginning. Successful saves checkpoint the WAL so
future recovery only replays post-save changes.

Transactions use transaction state tracking, commit-aware MVCC snapshot reads, and undo-log rollback
for DML. `BEGIN` opens a transaction, captures a commit-seq read snapshot, and clears the undo log
and any deferred WAL buffer; SELECTs use that snapshot (plus read-your-writes) so concurrent
uncommitted or post-`BEGIN` commits stay invisible; mutating `INSERT`/`UPDATE`/`DELETE` stamp SQL
transaction ids, append compensating undo records, and buffer physical redo records; `COMMIT` flushes
deferred redo as one atomic WAL batch then marks the transaction committed and discards the undo
log; `ROLLBACK` applies the undo log LIFO on the same database instance and drops deferred WAL.
While a transaction is active, `CREATE DATABASE`, `CREATE TABLE`, `DROP TABLE`, `RENAME TABLE`,
`CREATE INDEX`, `SAVE DATABASE`, and `LOAD DATABASE` are rejected.

## Types

- `INT`: stored as signed 64-bit integer.
- `DOUBLE`: stored as C++ `double`.
- `STRING`: stored as `std::string`.
- `NULL`: allowed only for columns declared nullable with `NULL`.

## Near-Term Grammar Work

- Better diagnostics with source positions.
- Aggregates such as `COUNT`.
- `GROUP BY`.
- Broader join syntax and multiple joins.
- Derived tables (`FROM (SELECT …)`), correlated subqueries, and `WITH … AS MATERIALIZED`.
- Expression indexes and specialized indexes for substring/regex predicates.
