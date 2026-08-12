#!/usr/bin/env sh
set -eu

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

configure_with_retry build-sanitize -DVERTEXDB_ENABLE_SANITIZERS=ON -DVERTEXDB_BUILD_TESTS=ON
cmake --build build-sanitize
ctest --test-dir build-sanitize --output-on-failure
