cmake_minimum_required(VERSION 3.24)

project(
    VertexDB
    VERSION 0.1.0
    DESCRIPTION "An educational C++20 in-memory relational database engine"
    LANGUAGES CXX
)

option(VERTEXDB_BUILD_TESTS "Build VertexDB tests" ON)
option(VERTEXDB_BUILD_BENCHMARKS "Build VertexDB benchmarks" OFF)
option(VERTEXDB_ENABLE_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(VERTEXDB_ENABLE_SANITIZERS "Enable address and undefined behavior sanitizers" OFF)
option(VERTEXDB_ENABLE_CLANG_TIDY "Run clang-tidy during compilation" OFF)
option(VERTEXDB_ENABLE_COVERAGE "Enable compiler coverage instrumentation" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)
include(GNUInstallDirs)

add_library(VertexDB
    src/common/value.cpp
    src/common/index_expression.cpp
    src/common/string_pattern.cpp
    src/parser/parser.cpp
    src/parser/parser_ddl.cpp
    src/parser/parser_dml.cpp
    src/parser/parser_select.cpp
    src/parser/parser_with.cpp
    src/parser/recursive_cte.cpp
    src/parser/parser_outer_refs.cpp
    src/parser/parser_predicate.cpp
    src/parser/tokenizer.cpp
    src/storage/database.cpp
    src/storage/buffer_pool.cpp
    src/storage/row_store_shared.cpp
    src/storage/vector_row_store.cpp
    src/storage/page_row_store.cpp
    src/storage/page_row_store_buffer.cpp
    src/storage/page_row_store_io.cpp
    src/storage/table.cpp
    src/storage/table_indexes.cpp
    src/storage/table_persist.cpp
    src/storage/table_snapshot_io.cpp
    src/storage/table_statistics.cpp
    src/storage/histogram.cpp
    src/indexing/btree_index_lookup.cpp
    src/indexing/btree_index_mutate.cpp
    src/indexing/btree_index_snapshot.cpp
    src/indexing/hash_index.cpp
    src/indexing/index_manager.cpp
    src/execution/query_executor.cpp
    src/execution/dml_engine.cpp
    src/execution/catalog_engine.cpp
    src/execution/select_engine.cpp
    src/execution/select_engine_scan.cpp
    src/execution/select_engine_join.cpp
    src/execution/txn_session.cpp
    src/execution/recovery_service.cpp
    src/execution/subquery_runtime.cpp
    src/execution/subquery_runtime_bind.cpp
    src/execution/subquery_runtime_cte.cpp
    src/execution/prepared_statement_catalog.cpp
    src/execution/predicate_eval.cpp
    src/execution/sql_literal.cpp
    src/execution/select_helpers.cpp
    src/execution/select_scope.cpp
    src/execution/select_aggregate.cpp
    src/execution/prepared_bind.cpp
    src/planner/query_planner.cpp
    src/planner/query_planner_select.cpp
    src/planner/query_planner_access.cpp
    src/planner/query_planner_predicate.cpp
    src/planner/query_planner_join.cpp
    src/planner/query_planner_format.cpp
    src/planner/rewriter.cpp
    src/concurrency/lock_manager.cpp
    src/persistence/storage_manager.cpp
    src/persistence/tcrdb_codec.cpp
    src/persistence/tcrdb_value_codec.cpp
    src/persistence/tcrdb_table_codec.cpp
    src/persistence/tcrdb_index_codec.cpp
    src/persistence/write_ahead_log.cpp
    src/persistence/physical_redo.cpp
    src/persistence/page_image_redo.cpp
    src/transaction/mvcc_row_store.cpp
    src/transaction/transaction_manager.cpp
    src/transaction/undo_log.cpp
)

add_library(VertexDB::VertexDB ALIAS VertexDB)

target_include_directories(VertexDB
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_compile_features(VertexDB PUBLIC cxx_std_20)

if(MSVC)
    target_compile_options(VertexDB PRIVATE /W4 /permissive-)
    if(VERTEXDB_ENABLE_WARNINGS_AS_ERRORS)
        target_compile_options(VertexDB PRIVATE /WX)
    endif()
else()
    target_compile_options(VertexDB PRIVATE -Wall -Wextra -Wpedantic)
    if(VERTEXDB_ENABLE_WARNINGS_AS_ERRORS)
        target_compile_options(VertexDB PRIVATE -Werror)
    endif()
endif()

if(VERTEXDB_ENABLE_SANITIZERS AND NOT MSVC)
    target_compile_options(VertexDB PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(VertexDB PUBLIC -fsanitize=address,undefined)
endif()

if(VERTEXDB_ENABLE_COVERAGE AND NOT MSVC)
    target_compile_options(VertexDB PUBLIC --coverage -O0 -g)
    target_link_options(VertexDB PUBLIC --coverage)
endif()

if(VERTEXDB_ENABLE_CLANG_TIDY)
    find_program(CLANG_TIDY_EXE NAMES clang-tidy REQUIRED)
    set_target_properties(VertexDB PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_EXE}")
endif()

add_executable(VertexDB_cli src/main.cpp)
target_link_libraries(VertexDB_cli PRIVATE VertexDB::VertexDB)

enable_testing()

if(VERTEXDB_BUILD_TESTS)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.14.0
        GIT_SHALLOW TRUE
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    add_executable(VertexDB_tests
        tests/test_support.cpp
        tests/parser_tests.cpp
        tests/storage_tests.cpp
        tests/index_tests.cpp
        tests/execution_tests.cpp
        tests/deep_feature_tests.cpp
        tests/nested_cte_tests.cpp
        tests/nested_correlation_tests.cpp
        tests/nested_subquery_tests.cpp
        tests/nested_recursive_tests.cpp
        tests/set_ops_tests.cpp
        tests/planner_access_tests.cpp
        tests/planner_intersect_union_tests.cpp
        tests/planner_explain_tests.cpp
        tests/planner_mutation_tests.cpp
        tests/planner_join_stats_tests.cpp
        tests/transaction_behavior_tests.cpp
        tests/persistence_behavior_tests.cpp
        tests/aggregate_prepared_tests.cpp
        tests/regression_tests.cpp
    )
    target_link_libraries(VertexDB_tests PRIVATE VertexDB::VertexDB GTest::gtest_main)
    include(GoogleTest)
    gtest_discover_tests(VertexDB_tests)
endif()

if(VERTEXDB_BUILD_BENCHMARKS)
    FetchContent_Declare(
        benchmark
        GIT_REPOSITORY https://github.com/google/benchmark.git
        GIT_TAG v1.8.3
        GIT_SHALLOW TRUE
    )
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(benchmark)

    add_executable(VertexDB_benchmarks benchmarks/storage_benchmarks.cpp)
    target_link_libraries(VertexDB_benchmarks PRIVATE VertexDB::VertexDB benchmark::benchmark_main)
endif()
