# Agent map

VertexDB is a C++20 educational in-memory SQL engine. Prefer extending existing
façade + focused-TU patterns over new layout conventions.

## Pipeline

```text
CLI → Parser → QueryExecutor → Planner / Storage / Indexes / Persistence / Txn
```

## Layout

| Dir | Owns |
| --- | --- |
| `include/VertexDB/<module>/` | Public headers (mirrors `src/`) |
| `src/common/` | Values, `IndexExpression`, string/binary helpers |
| `src/parser/` | Tokenizer + AST; one `Parser`, DDL/DML/predicate TUs |
| `src/planner/` | Rewriter + costed access paths / joins |
| `src/execution/` | `QueryExecutor` façade; `SelectEngine`, subquery, txn, recovery |
| `src/storage/` | `Database` / `Table` / `RowStore` / buffer pool / stats |
| `src/indexing/` | `IndexManager`, hash and B+ tree |
| `src/persistence/` | `.tcrdb` codecs, WAL, redo |
| `src/concurrency/` | `LockManager` |
| `src/transaction/` | Commit seq, MVCC, undo log |
| `tests/` | Themed GoogleTest suites (see `docs/testing.md`) |

**File name = owner type** when a type is split across TUs (e.g. `select_engine.cpp`
implements `SelectEngine`; `btree_index_mutate.cpp` is still `BTreeIndex`).

## Where to put new code

- New SQL syntax → parser TU + AST + tests; planner/executor only if semantics need it
- New access path → `query_planner*` + `SelectEngine` consumer + planner behavior test
- Storage/index internals → keep `Table` as the locked façade; put mechanics in collaborators
- Docs for behavior/SQL/tests belong in the same change (`docs/`, README counts)

## Deeper reading

- [docs/architecture.md](docs/architecture.md) — module responsibilities and data flow
- [docs/design.md](docs/design.md) — status, limitations, definition of done
- [docs/testing.md](docs/testing.md) — test file ownership and coverage floor
- [docs/sql.md](docs/sql.md) — SQL surface
