#!/usr/bin/env sh
# Build (optional) and run VertexDB Google Benchmarks with median sampling, then
# check wedge cost-shape ratios (CTE + multi-index intersect) from the same process.
#
# Usage:
#   scripts/run-benchmarks.sh                 # full report suite + shape check
#   scripts/run-benchmarks.sh --check-shape   # CTE + intersect benches (CI gate)
#   scripts/run-benchmarks.sh --skip-build    # use an existing binary
#
# Full-report JSON is the input for docs/benchmarks.md refresh:
#   python3 scripts/check_benchmark_shape.py --markdown-table "$VERTEXDB_BENCH_OUT/benchmark-report.json"
#
# Env:
#   VERTEXDB_BENCH_BIN, VERTEXDB_BENCH_BUILD_DIR, VERTEXDB_BENCH_OUT
#   VERTEXDB_BENCH_REPETITIONS (default 5)
#   VERTEXDB_BENCH_MIN_TIME (default 0.5s)

set -eu

ROOT="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${VERTEXDB_BENCH_BUILD_DIR:-$ROOT/build-benchmark}"
BIN="${VERTEXDB_BENCH_BIN:-$BUILD_DIR/VertexDB_benchmarks}"
OUT_DIR="${VERTEXDB_BENCH_OUT:-$BUILD_DIR}"
REPETITIONS="${VERTEXDB_BENCH_REPETITIONS:-5}"
MIN_TIME="${VERTEXDB_BENCH_MIN_TIME:-0.5s}"
PYTHON="${PYTHON:-python3}"

CHECK_SHAPE_ONLY=0
SKIP_BUILD=0

REPORT_FILTER='BM_IndexedPointLookup|BM_FilteredSelect|BM_NonIndexedFilteredSelect|BM_ConcurrentPointLookups|BM_VectorRowStore|BM_PageRowStore|BM_BTreeRangeQuery|BM_Transaction|BM_Cte|BM_MultiIndexIntersectSelect|BM_SingleIndexResidualSelect'
# `$` so /1000 does not also match materialized /10000.
SHAPE_FILTER='BM_CteIndexedWinSelect|BM_CteNonIndexedSelect|BM_CteMaterializedSelect/1000$|BM_MultiIndexIntersectSelect|BM_SingleIndexResidualSelect'

usage() {
    sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'
}

while [ "${1:-}" != "" ]; do
    case "$1" in
        --check-shape) CHECK_SHAPE_ONLY=1 ;;
        --skip-build) SKIP_BUILD=1 ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "unknown argument: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
    shift
done

if [ "$SKIP_BUILD" -eq 0 ]; then
    cmake -S "$ROOT" -B "$BUILD_DIR" \
        -DVERTEXDB_BUILD_TESTS=OFF \
        -DVERTEXDB_BUILD_BENCHMARKS=ON \
        -DCMAKE_BUILD_TYPE=Release
    cmake --build "$BUILD_DIR" --target VertexDB_benchmarks
fi

if [ ! -x "$BIN" ]; then
    echo "benchmark binary not found or not executable: $BIN" >&2
    echo "Build with: scripts/run-benchmarks.sh   (or pass VERTEXDB_BENCH_BIN / --skip-build)" >&2
    exit 1
fi

mkdir -p "$OUT_DIR"
if [ "$CHECK_SHAPE_ONLY" -eq 1 ]; then
    FILTER="$SHAPE_FILTER"
    JSON_OUT="$OUT_DIR/benchmark-shape.json"
else
    FILTER="$REPORT_FILTER"
    JSON_OUT="$OUT_DIR/benchmark-report.json"
fi

echo "Running $BIN"
echo "  filter:       $FILTER"
echo "  repetitions:  $REPETITIONS"
echo "  min_time:     $MIN_TIME"
echo "  json:         $JSON_OUT"

"$BIN" \
    --benchmark_filter="$FILTER" \
    --benchmark_repetitions="$REPETITIONS" \
    --benchmark_report_aggregates_only=true \
    --benchmark_min_time="$MIN_TIME" \
    --benchmark_enable_random_interleaving=true \
    --benchmark_out="$JSON_OUT" \
    --benchmark_out_format=json

"$PYTHON" "$ROOT/scripts/check_benchmark_shape.py" "$JSON_OUT"
