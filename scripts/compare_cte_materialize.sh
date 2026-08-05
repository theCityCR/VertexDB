#!/usr/bin/env sh
# Compare the CTE index-win query: VertexDB (always inlines) vs Postgres
# WITH … AS MATERIALIZED / AS NOT MATERIALIZED.
#
# Usage:
#   scripts/compare_cte_materialize.sh
#   ROWS=10000 CLI=./build/VertexDB_cli scripts/compare_cte_materialize.sh
#
# Requires: VertexDB_cli (build first). Postgres half needs Docker with a working daemon
# (or PGHOST/PGUSER/PGDATABASE pointing at an existing server with psql on PATH).

set -eu

ROOT="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
ROWS="${ROWS:-10000}"
CLI="${CLI:-$ROOT/build/VertexDB_cli}"
PG_IMAGE="${PG_IMAGE:-postgres:16}"
CONTAINER="${CONTAINER:-vertexdb-cte-pg}"
PG_PORT="${PG_PORT:-55432}"
OUT="${OUT:-}"
KEEP_CONTAINER="${KEEP_CONTAINER:-0}"

win_sql_pg() {
    cat <<'SQL'
EXPLAIN (COSTS OFF)
WITH high AS MATERIALIZED (
  SELECT id, name, salary FROM employees WHERE salary > 100000.0
)
SELECT name FROM high WHERE id = 1;
SQL
}

win_sql_pg_inline() {
    cat <<'SQL'
EXPLAIN (COSTS OFF)
WITH high AS NOT MATERIALIZED (
  SELECT id, name, salary FROM employees WHERE salary > 100000.0
)
SELECT name FROM high WHERE id = 1;
SQL
}

seed_pg_sql() {
    cat <<SQL
DROP TABLE IF EXISTS employees;
CREATE TABLE employees (
  id integer PRIMARY KEY,
  name text NOT NULL,
  salary double precision NOT NULL
);
INSERT INTO employees (id, name, salary) VALUES (1, 'Alice', 120000.0);
INSERT INTO employees (id, name, salary)
SELECT g, 'Emp' || g, CASE WHEN g % 20 = 0 THEN 80000.0 ELSE 110000.0 END
FROM generate_series(2, ${ROWS}) AS g;
ANALYZE employees;
SQL
}

section() {
    printf '\n## %s\n\n' "$1"
}

have_docker_postgres() {
    command -v docker >/dev/null 2>&1 || return 1
    docker info >/dev/null 2>&1 || return 1
    return 0
}

have_local_psql() {
    command -v psql >/dev/null 2>&1 || return 1
    psql -c 'SELECT 1' >/dev/null 2>&1 || return 1
    return 0
}

run_vertexdb() {
    section "VertexDB (always-inline CTEs)"

    if [ ! -x "$CLI" ]; then
        echo "VertexDB CLI not found at $CLI"
        echo "Build with: cmake -S . -B build && cmake --build build --target VertexDB_cli"
        return 1
    fi

    work="$(mktemp -d "${TMPDIR:-/tmp}/vertexdb-cte-cmp.XXXXXX")"
    cleanup_vd() { rm -rf "$work"; }
    trap cleanup_vd EXIT

    # CLI is line-oriented; seed with a compact INSERT then rely on docs/examples for large N.
    {
        echo 'CREATE DATABASE cte_cmp;'
        echo 'CREATE TABLE Employees (id INT, name STRING, salary DOUBLE);'
        echo 'CREATE INDEX idx_id ON Employees(id);'
        echo 'INSERT INTO Employees VALUES (1, "Alice", 120000.0), (2, "Bob", 110000.0), (3, "Cara", 90000.0);'
        echo 'EXPLAIN WITH high AS (SELECT id, name, salary FROM Employees WHERE salary > 100000.0) SELECT name FROM high WHERE id = 1;'
        echo 'WITH high AS (SELECT id, name, salary FROM Employees WHERE salary > 100000.0) SELECT name FROM high WHERE id = 1;'
        echo 'EXIT;'
    } >"$work/seed.sql"

    echo "CLI: $CLI"
    echo 'Query: WITH high AS (... salary > 100000) SELECT name FROM high WHERE id = 1;'
    echo
    echo '```text'
    (cd "$work" && "$CLI" <"$work/seed.sql") | sed -n '/^plan/,/^VertexDB> selected/p' | sed '/^VertexDB> selected/d'
    echo '```'
    echo
    echo 'Expected: hash index equality lookup on `id`, residual salary filter, `inlined CTE high`.'
    echo "For 1k/100k timings see \`BM_CteIndexedWinSelect\` vs \`BM_CteNonIndexedSelect\`."

    trap - EXIT
    cleanup_vd
}

start_docker_pg() {
    docker rm -f "$CONTAINER" >/dev/null 2>&1 || true
    docker run -d --name "$CONTAINER" \
        -e POSTGRES_PASSWORD=postgres \
        -e POSTGRES_USER=postgres \
        -e POSTGRES_DB=cte_cmp \
        -p "${PG_PORT}:5432" \
        "$PG_IMAGE" >/dev/null

    i=0
    while [ "$i" -lt 60 ]; do
        if docker exec "$CONTAINER" pg_isready -U postgres >/dev/null 2>&1; then
            return 0
        fi
        i=$((i + 1))
        sleep 1
    done
    echo "Postgres container did not become ready" >&2
    return 1
}

run_postgres_docker() {
    section "Postgres via Docker (${PG_IMAGE}, ${ROWS} rows)"

    start_docker_pg
    docker exec -i "$CONTAINER" psql -U postgres -d cte_cmp -v ON_ERROR_STOP=1 <<<"$(seed_pg_sql)" >/dev/null

    echo '### AS MATERIALIZED'
    echo
    echo '```text'
    docker exec -i "$CONTAINER" psql -U postgres -d cte_cmp -v ON_ERROR_STOP=1 <<<"$(win_sql_pg)"
    echo '```'
    echo
    echo '### AS NOT MATERIALIZED'
    echo
    echo '```text'
    docker exec -i "$CONTAINER" psql -U postgres -d cte_cmp -v ON_ERROR_STOP=1 <<<"$(win_sql_pg_inline)"
    echo '```'
    echo
    echo 'Look for: MATERIALIZED typically shows a CTE Scan over a scanned CTE result;'
    echo 'NOT MATERIALIZED can collapse to an index probe on `employees(id)` with a salary filter.'

    if [ "$KEEP_CONTAINER" != 1 ]; then
        docker rm -f "$CONTAINER" >/dev/null 2>&1 || true
    else
        echo "Left container running: $CONTAINER (port $PG_PORT)"
    fi
}

run_postgres_local() {
    section "Postgres via local psql (${ROWS} rows)"

    psql -v ON_ERROR_STOP=1 <<<"$(seed_pg_sql)" >/dev/null

    echo '### AS MATERIALIZED'
    echo
    echo '```text'
    psql -v ON_ERROR_STOP=1 <<<"$(win_sql_pg)"
    echo '```'
    echo
    echo '### AS NOT MATERIALIZED'
    echo
    echo '```text'
    psql -v ON_ERROR_STOP=1 <<<"$(win_sql_pg_inline)"
    echo '```'
}

main() {
    report="$(mktemp "${TMPDIR:-/tmp}/vertexdb-cte-report.XXXXXX.md")"
    {
        echo '# CTE materialize vs inline comparison'
        echo
        echo "Generated by \`scripts/compare_cte_materialize.sh\` (ROWS=${ROWS})."
        echo
        echo 'Win query:'
        echo
        echo '```sql'
        echo 'WITH high AS ('
        echo '  SELECT id, name, salary FROM Employees WHERE salary > 100000.0'
        echo ')'
        echo 'SELECT name FROM high WHERE id = 1;'
        echo '```'

        run_vertexdb

        if have_docker_postgres; then
            run_postgres_docker
        elif have_local_psql; then
            run_postgres_local
        else
            section "Postgres (skipped)"
            echo 'Neither Docker (daemon up) nor a working local `psql` was available.'
            echo 'Start Docker Desktop (or set PG* + install psql), then re-run this script.'
            echo 'Illustrative MATERIALIZED vs NOT MATERIALIZED plan shapes are in'
            echo 'docs/cte_materialize_comparison.md.'
        fi
    } >"$report"

    if [ -n "$OUT" ]; then
        cp "$report" "$OUT"
        echo "Wrote $OUT"
    fi
    cat "$report"
    rm -f "$report"
}

main "$@"
