#!/usr/bin/env sh
set -eu

MIN_COVERAGE="${VERTEXDB_COVERAGE_MIN:-85}"
PROJECT_ROOT="$(pwd)"

# Retry configure: FetchContent downloads of googletest occasionally fail with
# transient GitHub 503 / HTTP2 REFUSED_STREAM on CI runners.
configure_with_retry() {
    build_dir="$1"
    shift
    attempt=1
    max_attempts=5
    while true; do
        if cmake -S . -B "$build_dir" "$@"; then
            return 0
        fi
        if [ "$attempt" -ge "$max_attempts" ]; then
            echo "cmake configure failed after ${max_attempts} attempts" >&2
            return 1
        fi
        echo "cmake configure failed (attempt ${attempt}/${max_attempts}); retrying..." >&2
        rm -rf "$build_dir"
        attempt=$((attempt + 1))
        sleep $((attempt * 2))
    done
}

configure_with_retry build-coverage -DVERTEXDB_ENABLE_COVERAGE=ON -DVERTEXDB_BUILD_TESTS=ON
find build-coverage -name '*.gcda' -delete
cmake --build build-coverage
find build-coverage -name '*.gcda' -delete
ctest --test-dir build-coverage --output-on-failure

coverage_dir="$(mktemp -d)"
trap 'rm -rf "$coverage_dir"' EXIT

for source in "$PROJECT_ROOT"/src/*/*.cpp; do
    relative_source="${source#$PROJECT_ROOT/}"
    notes_file="$PROJECT_ROOT/build-coverage/CMakeFiles/VertexDB.dir/$relative_source.gcno"
    if [ -f "$notes_file" ]; then
        (
            cd "$coverage_dir"
            gcov -o "$notes_file" "$source" > /dev/null
        )
    fi
done

coverage_summary="$(
    awk '
        /^[[:space:]]*[0-9]+:/ { covered += 1 }
        /^[[:space:]]*#####:/ { missed += 1 }
        END {
            total = covered + missed
            if (total == 0) {
                print "0 0 0 0"
            } else {
                printf "%.2f %d %d %d\n", (covered * 100.0) / total, covered, missed, total
            }
        }
    ' "$coverage_dir"/*.cpp.gcov
)"

coverage_percent="$(printf '%s' "$coverage_summary" | awk '{ print $1 }')"
covered_lines="$(printf '%s' "$coverage_summary" | awk '{ print $2 }')"
missed_lines="$(printf '%s' "$coverage_summary" | awk '{ print $3 }')"
total_lines="$(printf '%s' "$coverage_summary" | awk '{ print $4 }')"

printf 'Line coverage: %s%% (%s/%s covered, %s missed)\n' \
    "$coverage_percent" "$covered_lines" "$total_lines" "$missed_lines"

awk -v actual="$coverage_percent" -v required="$MIN_COVERAGE" '
    BEGIN {
        if (actual + 0 < required + 0) {
            printf "Coverage %.2f%% is below required %.2f%%\n", actual, required
            exit 1
        }
    }
'
