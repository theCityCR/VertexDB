#include "VertexDB/storage/table.hpp"

#include "VertexDB/common/comparison_operator.hpp"
#include "VertexDB/execution/query_executor.hpp"
#include "VertexDB/parser/parser.hpp"
#include "VertexDB/storage/row_store.hpp"

#include <benchmark/benchmark.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <future>
#include <string>
#include <thread>

namespace VertexDB {
namespace {

// Never use the default "data/" root — shared WAL across iterations balloons disk and can OOM the
// host. Each executor gets an isolated temp directory that is removed when the guard dies.
struct TempExecutor {
    std::filesystem::path root;
    QueryExecutor executor;

    TempExecutor()
        : root(std::filesystem::temp_directory_path() /
               ("vertexdb-bench-" +
                std::to_string(
                    std::chrono::steady_clock::now().time_since_epoch().count()) +
                "-" + std::to_string(reinterpret_cast<std::uintptr_t>(this)))),
          executor(root) {
        std::filesystem::create_directories(root);
    }

    ~TempExecutor() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    TempExecutor(const TempExecutor &) = delete;
    TempExecutor &operator=(const TempExecutor &) = delete;
};

void BM_InsertRows(benchmark::State &state) {
    for (auto _ : state) {
        Table table{"Employees", {{"id", ColumnType::Int}, {"name", ColumnType::String}}};
        for (std::int64_t i = 0; i < state.range(0); ++i) {
            table.insert({Value{i}, Value{std::string{"employee"}}});
        }
        benchmark::DoNotOptimize(table.rowCount());
    }
}

BENCHMARK(BM_InsertRows)->Arg(1000)->Arg(100000);

void BM_IndexedPointLookup(benchmark::State &state) {
    Table table{"Employees", {{"id", ColumnType::Int}, {"name", ColumnType::String}}};
    for (std::int64_t i = 0; i < state.range(0); ++i) {
        table.insert({Value{i}, Value{std::string{"employee"}}});
    }
    table.createIndex("idx_id", "id");

    for (auto _ : state) {
        auto rows = table.indexedLookup("id", Value{static_cast<std::int64_t>(state.range(0) / 2)})
                        .value_or(std::vector<RowId>{});
        benchmark::DoNotOptimize(rows);
    }
}

BENCHMARK(BM_IndexedPointLookup)->Arg(1000)->Arg(100000);

void BM_FilteredSelect(benchmark::State &state) {
    Parser parser;
    TempExecutor env;
    (void)env.executor.execute(parser.parse("CREATE DATABASE bench;"));
    (void)env.executor.execute(parser.parse("CREATE TABLE Employees (id INT, name STRING);"));
    (void)env.executor.execute(parser.parse("CREATE INDEX idx_id ON Employees(id);"));
    auto table = env.executor.currentDatabase()->table("Employees");
    for (std::int64_t i = 0; i < state.range(0); ++i) {
        table->insert({Value{i}, Value{std::string{"employee"}}});
    }
    const auto query = parser.parse("SELECT name FROM Employees WHERE id = 500;");

    for (auto _ : state) {
        auto result = env.executor.execute(query);
        benchmark::DoNotOptimize(result);
    }
}

BENCHMARK(BM_FilteredSelect)->Arg(1000)->Arg(100000);

void BM_NonIndexedFilteredSelect(benchmark::State &state) {
    Parser parser;
    TempExecutor env;
    (void)env.executor.execute(parser.parse("CREATE DATABASE bench;"));
    (void)env.executor.execute(parser.parse("CREATE TABLE Employees (id INT, name STRING);"));
    auto table = env.executor.currentDatabase()->table("Employees");
    for (std::int64_t i = 0; i < state.range(0); ++i) {
        table->insert({Value{i}, Value{std::string{"employee"}}});
    }
    const auto query = parser.parse("SELECT name FROM Employees WHERE id = 500;");

    for (auto _ : state) {
        auto result = env.executor.execute(query);
        benchmark::DoNotOptimize(result);
    }
}

BENCHMARK(BM_NonIndexedFilteredSelect)->Arg(1000)->Arg(100000);

void BM_UpdateRows(benchmark::State &state) {
    Parser parser;
    for (auto _ : state) {
        state.PauseTiming();
        TempExecutor env;
        (void)env.executor.execute(parser.parse("CREATE DATABASE bench;"));
        (void)env.executor.execute(parser.parse("CREATE TABLE Employees (id INT, salary DOUBLE);"));
        auto table = env.executor.currentDatabase()->table("Employees");
        for (std::int64_t i = 0; i < state.range(0); ++i) {
            table->insert({Value{i}, Value{100000.0}});
        }
        const auto query = parser.parse("UPDATE Employees SET salary = 125000.0 WHERE id > -1;");
        state.ResumeTiming();

        auto result = env.executor.execute(query);
        benchmark::DoNotOptimize(result);
    }
}

BENCHMARK(BM_UpdateRows)->Arg(1000)->Arg(10000);

void BM_DeleteRows(benchmark::State &state) {
    Parser parser;
    for (auto _ : state) {
        state.PauseTiming();
        TempExecutor env;
        (void)env.executor.execute(parser.parse("CREATE DATABASE bench;"));
        (void)env.executor.execute(parser.parse("CREATE TABLE Employees (id INT);"));
        auto table = env.executor.currentDatabase()->table("Employees");
        for (std::int64_t i = 0; i < state.range(0); ++i) {
            table->insert({Value{i}});
        }
        const auto query = parser.parse("DELETE FROM Employees WHERE id > -1;");
        state.ResumeTiming();

        auto result = env.executor.execute(query);
        benchmark::DoNotOptimize(result);
    }
}

BENCHMARK(BM_DeleteRows)->Arg(1000)->Arg(10000);

void BM_ConcurrentPointLookups(benchmark::State &state) {
    Parser parser;
    TempExecutor env;
    (void)env.executor.execute(parser.parse("CREATE DATABASE bench;"));
    (void)env.executor.execute(parser.parse("CREATE TABLE Employees (id INT, name STRING);"));
    (void)env.executor.execute(parser.parse("CREATE INDEX idx_id ON Employees(id);"));
    auto table = env.executor.currentDatabase()->table("Employees");
    for (std::int64_t i = 0; i < 10000; ++i) {
        table->insert({Value{i}, Value{std::string{"employee"}}});
    }
    const auto query = parser.parse("SELECT name FROM Employees WHERE id = 5000;");
    const auto workers = static_cast<int>(state.range(0));

    for (auto _ : state) {
        std::vector<std::future<QueryResult>> futures;
        futures.reserve(static_cast<std::size_t>(workers));
        for (int worker = 0; worker < workers; ++worker) {
            futures.push_back(std::async(std::launch::async, [&env, &query] {
                return env.executor.execute(query);
            }));
        }
        for (auto &future : futures) {
            auto result = future.get();
            benchmark::DoNotOptimize(result);
        }
    }
}

BENCHMARK(BM_ConcurrentPointLookups)
    ->Arg(1)
    ->Arg(2)
    ->Arg(static_cast<int>(std::thread::hardware_concurrency() == 0
                               ? 4
                               : std::thread::hardware_concurrency()));

void seedCteWinEmployees(QueryExecutor &executor, std::int64_t rowCount, bool indexId) {
    Parser parser;
    (void)executor.execute(parser.parse("CREATE DATABASE bench;"));
    (void)executor.execute(
        parser.parse("CREATE TABLE Employees (id INT, name STRING, salary DOUBLE);"));

    auto table = executor.currentDatabase()->table("Employees");
    table->insert({Value{static_cast<std::int64_t>(1)}, Value{std::string{"Alice"}}, Value{120000.0}});
    for (std::int64_t id = 2; id <= rowCount; ++id) {
        // Most rows match the CTE body filter; a materializing engine would build a large temp.
        const double salary = (id % 20 == 0) ? 80000.0 : 110000.0;
        table->insert({Value{id}, Value{std::string{"Emp"}}, Value{salary}});
    }
    if (indexId) {
        (void)table->createIndex("idx_id", "id");
    }
}

// Inlined CTE + outer equality: expected plan is hash index on id with salary residual.
void BM_CteIndexedWinSelect(benchmark::State &state) {
    TempExecutor env;
    seedCteWinEmployees(env.executor, state.range(0), true);

    Parser parser;
    const auto query = parser.parse(
        "WITH high AS (SELECT id, name, salary FROM Employees WHERE salary > 100000.0) "
        "SELECT name FROM high WHERE id = 1;");

    for (auto _ : state) {
        auto result = env.executor.execute(query);
        benchmark::DoNotOptimize(result);
    }
}

BENCHMARK(BM_CteIndexedWinSelect)->Arg(1000)->Arg(100000);

// Same CTE SQL without an id index: expected plan is a full table scan.
void BM_CteNonIndexedSelect(benchmark::State &state) {
    TempExecutor env;
    seedCteWinEmployees(env.executor, state.range(0), false);

    Parser parser;
    const auto query = parser.parse(
        "WITH high AS (SELECT id, name, salary FROM Employees WHERE salary > 100000.0) "
        "SELECT name FROM high WHERE id = 1;");

    for (auto _ : state) {
        auto result = env.executor.execute(query);
        benchmark::DoNotOptimize(result);
    }
}

BENCHMARK(BM_CteNonIndexedSelect)->Arg(1000)->Arg(100000);

void seedIntersectEmployees(QueryExecutor &executor, std::int64_t rowCount, bool indexCity) {
    Parser parser;
    (void)executor.execute(parser.parse("CREATE DATABASE bench;"));
    (void)executor.execute(
        parser.parse("CREATE TABLE Employees (id INT, dept INT, city INT, name STRING);"));

    auto table = executor.currentDatabase()->table("Employees");
    for (std::int64_t id = 1; id <= rowCount; ++id) {
        const auto dept = static_cast<std::int64_t>(id % 10);
        const auto city = static_cast<std::int64_t>((id / 10) % 10);
        table->insert({Value{id}, Value{dept}, Value{city}, Value{std::string{"Emp"}}});
    }
    (void)table->createIndex("idx_dept", "dept");
    if (indexCity) {
        (void)table->createIndex("idx_city", "city");
    }
}

// Two medium-cardinality equality indexes: expected plan is multi-index intersect.
void BM_MultiIndexIntersectSelect(benchmark::State &state) {
    TempExecutor env;
    seedIntersectEmployees(env.executor, state.range(0), true);

    Parser parser;
    const auto query =
        parser.parse("SELECT name FROM Employees WHERE dept = 1 AND city = 1;");

    for (auto _ : state) {
        auto result = env.executor.execute(query);
        benchmark::DoNotOptimize(result);
    }
}

BENCHMARK(BM_MultiIndexIntersectSelect)->Arg(1000)->Arg(100000);

// Same SQL with only dept indexed: expected plan is hash equality + residual city filter.
void BM_SingleIndexResidualSelect(benchmark::State &state) {
    TempExecutor env;
    seedIntersectEmployees(env.executor, state.range(0), false);

    Parser parser;
    const auto query =
        parser.parse("SELECT name FROM Employees WHERE dept = 1 AND city = 1;");

    for (auto _ : state) {
        auto result = env.executor.execute(query);
        benchmark::DoNotOptimize(result);
    }
}

BENCHMARK(BM_SingleIndexResidualSelect)->Arg(1000)->Arg(100000);

// Page-backed vs vector-backed row-store insert throughput (storage layer).
void BM_VectorRowStoreInsert(benchmark::State &state) {
    for (auto _ : state) {
        auto store = makeVectorRowStore();
        for (std::int64_t i = 0; i < state.range(0); ++i) {
            (void)store->append({Value{i}, Value{std::string{"employee"}}});
        }
        benchmark::DoNotOptimize(store->size());
    }
}

BENCHMARK(BM_VectorRowStoreInsert)->Arg(1000)->Arg(100000);

void BM_PageRowStoreInsert(benchmark::State &state) {
    for (auto _ : state) {
        auto store = makePageRowStore();
        for (std::int64_t i = 0; i < state.range(0); ++i) {
            (void)store->append({Value{i}, Value{std::string{"employee"}}});
        }
        benchmark::DoNotOptimize(store->size());
    }
}

// Cap at 10k: 100k page-store inserts dominate wall time (~10s+/iter) without adding signal.
BENCHMARK(BM_PageRowStoreInsert)->Arg(1000)->Arg(10000);

// Mid-row get after fill — page buffer hit vs contiguous vector.
void BM_VectorRowStoreSelect(benchmark::State &state) {
    auto store = makeVectorRowStore();
    for (std::int64_t i = 0; i < state.range(0); ++i) {
        (void)store->append({Value{i}, Value{std::string{"employee"}}});
    }
    const RowId mid = static_cast<RowId>(state.range(0) / 2);

    for (auto _ : state) {
        const auto *row = store->get(mid);
        benchmark::DoNotOptimize(row);
    }
}

BENCHMARK(BM_VectorRowStoreSelect)->Arg(1000)->Arg(100000);

void BM_PageRowStoreSelect(benchmark::State &state) {
    auto store = makePageRowStore();
    for (std::int64_t i = 0; i < state.range(0); ++i) {
        (void)store->append({Value{i}, Value{std::string{"employee"}}});
    }
    const RowId mid = static_cast<RowId>(state.range(0) / 2);

    for (auto _ : state) {
        const auto *row = store->get(mid);
        benchmark::DoNotOptimize(row);
    }
}

BENCHMARK(BM_PageRowStoreSelect)->Arg(1000)->Arg(100000);

// B+ tree ordered range (id > mid) via Table::orderedLookup.
void BM_BTreeRangeQuery(benchmark::State &state) {
    Table table{"Employees", {{"id", ColumnType::Int}, {"name", ColumnType::String}}};
    for (std::int64_t i = 0; i < state.range(0); ++i) {
        table.insert({Value{i}, Value{std::string{"employee"}}});
    }
    table.createIndex("idx_id", "id");
    const Value mid{static_cast<std::int64_t>(state.range(0) / 2)};

    for (auto _ : state) {
        auto rows =
            table.orderedLookup("id", ComparisonOperator::Greater, mid).value_or(std::vector<RowId>{});
        benchmark::DoNotOptimize(rows);
    }
}

BENCHMARK(BM_BTreeRangeQuery)->Arg(1000)->Arg(100000);

// Snapshot-isolation read under an open transaction (SQL BEGIN + SELECT).
void BM_TransactionSnapshotRead(benchmark::State &state) {
    Parser parser;
    TempExecutor env;
    (void)env.executor.execute(parser.parse("CREATE DATABASE bench;"));
    (void)env.executor.execute(parser.parse("CREATE TABLE Employees (id INT, name STRING);"));
    (void)env.executor.execute(parser.parse("CREATE INDEX idx_id ON Employees(id);"));
    auto table = env.executor.currentDatabase()->table("Employees");
    for (std::int64_t i = 0; i < state.range(0); ++i) {
        table->insert({Value{i}, Value{std::string{"employee"}}});
    }
    (void)env.executor.execute(parser.parse("BEGIN;"));
    const auto query = parser.parse("SELECT name FROM Employees WHERE id = 500;");

    for (auto _ : state) {
        auto result = env.executor.execute(query);
        benchmark::DoNotOptimize(result);
    }
    (void)env.executor.execute(parser.parse("ROLLBACK;"));
}

BENCHMARK(BM_TransactionSnapshotRead)->Arg(1000)->Arg(10000);

// BEGIN + N inserts + ROLLBACK (undo path) — timed per iteration.
void BM_TransactionRollback(benchmark::State &state) {
    Parser parser;
    for (auto _ : state) {
        state.PauseTiming();
        TempExecutor env;
        (void)env.executor.execute(parser.parse("CREATE DATABASE bench;"));
        (void)env.executor.execute(parser.parse("CREATE TABLE Employees (id INT, name STRING);"));
        auto table = env.executor.currentDatabase()->table("Employees");
        for (std::int64_t i = 0; i < 100; ++i) {
            table->insert({Value{i}, Value{std::string{"base"}}});
        }
        state.ResumeTiming();

        (void)env.executor.execute(parser.parse("BEGIN;"));
        for (std::int64_t i = 0; i < state.range(0); ++i) {
            (void)env.executor.execute(
                Insert{"Employees", {{Value{1000 + i}, Value{std::string{"tx"}}}}});
        }
        auto result = env.executor.execute(parser.parse("ROLLBACK;"));
        benchmark::DoNotOptimize(result);
    }
}

BENCHMARK(BM_TransactionRollback)->Arg(100)->Arg(1000);

// MATERIALIZED CTE fences the body into a temp; outer filter cannot use base-table idx_id.
// Caps at 10k rows: each iteration builds the temp, so 100k would dominate wall time.
void BM_CteMaterializedSelect(benchmark::State &state) {
    TempExecutor env;
    seedCteWinEmployees(env.executor, state.range(0), true);

    Parser parser;
    const auto query = parser.parse(
        "WITH high AS MATERIALIZED (SELECT id, name, salary FROM Employees WHERE salary > 100000.0) "
        "SELECT name FROM high WHERE id = 1;");

    for (auto _ : state) {
        auto result = env.executor.execute(query);
        benchmark::DoNotOptimize(result);
    }
}

BENCHMARK(BM_CteMaterializedSelect)->Arg(1000)->Arg(10000);

} // namespace
} // namespace VertexDB
