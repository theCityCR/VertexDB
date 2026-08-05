#!/usr/bin/env sh
set -eu

cmake -S . -B build-sanitize -DVERTEXDB_ENABLE_SANITIZERS=ON -DVERTEXDB_BUILD_TESTS=ON
cmake --build build-sanitize
ctest --test-dir build-sanitize --output-on-failure
