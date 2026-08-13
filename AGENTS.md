# Agent map

VertexDB is a C++20 educational in-memory SQL engine. Prefer extending existing
façade + focused-TU patterns over new layout conventions.

## Pipeline

```text
CLI → Parser → QueryExecutor
         → Rewriter (CTE / derived / IN prep)
         → QueryPlanner (access path + joins)
         → Engines via ExecutionContext
              SelectEngine / SubqueryRuntime / DmlEngine / CatalogEngine
              + TxnSession / RecoveryService
```

UPDATE/DELETE reuse `QueryPlanner::planSelect` plus `SelectEngine::collectVisibleEntries`
so mutation `WHERE` clauses use the same index access paths as SELECT.

## Layout

| Dir | Owns |
| --- | --- |
| `include/VertexDB/<module>/` | Public headers (mirrors `src/`) |
| `src/common/` | Values, `IndexExpression`, string/binary helpers |
| `src/parser/` | Tokenizer + AST; one `Parser`, DDL/DML/SELECT/WITH/outer-refs/predicate TUs |
| `src/planner/` | Rewriter + costed access paths / joins (`query_planner_select.cpp` orchestrates `planSelect`; OR-union / composite HashEq / AND-intersect / finalize live under `query_planner_access.hpp` + sibling TUs; `query_planner_predicate.cpp` owns predicate trees/costing) |
| `src/execution/` | `QueryExecutor` façade; `ExecutionContext`; `SelectEngine` (+ scan/join/SSI/bitmap), `DmlEngine`, `CatalogEngine`, subquery (+ bind/cte TUs), txn, recovery; helpers include `foreign_key_eval`, `set_ops` |
| `src/storage/` | `Database` / `Table` (+ constraints/schema/recovery TUs) / `RowStore` (+ `PageRowStore` CRUD/buffer/io TUs, `VectorRowStore`) / buffer pool / stats |
| `src/indexing/` | `IndexManager`, hash and B+ tree |
| `src/persistence/` | `.tcrdb` codecs, WAL, redo |
| `src/concurrency/` | `LockManager` |
| `src/transaction/` | Commit seq, MVCC, undo log |
| `tests/` | Themed GoogleTest suites (see `docs/testing.md`) |

**File name = owner type** when a type is split across TUs (e.g. `select_engine.cpp`
implements `SelectEngine`; `btree_index_mutate.cpp` is still `BTreeIndex`).

`SelectEngine`, `SubqueryRuntime`, `DmlEngine`, and `CatalogEngine` share an `ExecutionContext`
(database, planner, session, peer pointers). They do **not** friend `QueryExecutor`.

## Ownership cheat sheet

| Concern | Open first |
| --- | --- |
| OR-union / AND-intersect / composite HashEq strategies | `src/planner/query_planner_access.hpp` (+ `query_planner_or_union.cpp`, `query_planner_and_intersect.cpp`, `query_planner_composite_eq.cpp`) |
| WHERE residual filters (non-subquery) | `execution/predicate_eval` |
| CHECK constraints (3-valued) | `storage/check_eval` |
| Correlated `IN`/`EXISTS` / full predicate match | `SubqueryRuntime::matches` (scan/join call it via `ExecutionContext`) |
| Recursive CTE grammar / caps | `parser/recursive_cte` |
| Recursive CTE materialization | `subquery_runtime_cte.cpp` |
| FK metadata | `storage/foreign_key.hpp` on `Table` |
| FK RI actions (CASCADE / SET NULL / NO ACTION) | `execution/foreign_key_eval` |
| Plan residual vs complementary residual | `PlanEstimates`: top-level partial OR uses `residual`; nested partial OR under AND uses `complementaryResidual` so AND residual filtering stays distinct |
| AccessPath names | `Intersect` / `Union` / `HashEq` (benchmarks may still say `BM_MultiIndexIntersectSelect` — CI shape gate alias) |

## Where to put new code

- New SQL syntax → parser TU + AST + tests; planner/executor only if semantics need it
- New access path → `query_planner*` (+ access strategy TU) + `SelectEngine` consumer + planner behavior test
- Storage/index internals → keep `Table` as the locked façade; put mechanics in `table_*.cpp` / collaborators
- Docs for behavior/SQL/tests belong in the same change (`docs/`, README counts)

## Deeper reading

- [docs/architecture.md](docs/architecture.md) — module responsibilities and data flow
- [docs/design.md](docs/design.md) — status, limitations, ACID plan, definition of done
- [docs/testing.md](docs/testing.md) — test file ownership and coverage floor
- [docs/sql.md](docs/sql.md) — SQL surface
- Portfolio wedges: [cte_index_wedge.md](docs/cte_index_wedge.md),
  [multi_index_intersect_wedge.md](docs/multi_index_intersect_wedge.md),
  [si_anomaly_wedge.md](docs/si_anomaly_wedge.md)
