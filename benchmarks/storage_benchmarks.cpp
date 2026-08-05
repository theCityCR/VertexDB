#include "VertexDB/storage/table.hpp"

#include "VertexDB/execution/query_executor.hpp"
#include "VertexDB/parser/parser.hpp"

#include <benchmark/benchmark.h>

#include <future>
#include <thread>

namespace VertexDB {
namespace {

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
        auto rows = table.findIndexed("id", Value{static_cast<std::int64_t>(state.range(0) / 2)});
        benchmark::DoNotOptimize(rows);
    }
}

BENCHMARK(BM_IndexedPointLookup)->Arg(1000)->Arg(100000);

void BM_FilteredSelect(benchmark::State &state) {
    Parser parser;
    QueryExecutor executor;
    (void)executor.execute(parser.parse("CREATE DATABASE bench;"));
    (void)executor.execute(parser.parse("CREATE TABLE Employees (id INT, name STRING);"));
    (void)executor.execute(parser.parse("CREATE INDEX idx_id ON Employees(id);"));
    for (std::int64_t i = 0; i < state.range(0); ++i) {
        (void)executor.execute(Insert{"Employees", {{Value{i}, Value{std::string{"employee"}}}}});
    }
    const auto query = parser.parse("SELECT name FROM Employees WHERE id = 500;");

    for (auto _ : state) {
        auto result = executor.execute(query);
        benchmark::DoNotOptimize(result);
    }
}

BENCHMARK(BM_FilteredSelect)->Arg(1000)->Arg(100000);

void BM_NonIndexedFilteredSelect(benchmark::State &state) {
    Parser parser;
    QueryExecutor executor;
    (void)executor.execute(parser.parse("CREATE DATABASE bench;"));
    (void)executor.execute(parser.parse("CREATE TABLE Employees (id INT, name STRING);"));
    for (std::int64_t i = 0; i < state.range(0); ++i) {
        (void)executor.execute(Insert{"Employees", {{Value{i}, Value{std::string{"employee"}}}}});
    }
    const auto query = parser.parse("SELECT name FROM Employees WHERE id = 500;");

    for (auto _ : state) {
        auto result = executor.execute(query);
        benchmark::DoNotOptimize(result);
    }
}

BENCHMARK(BM_NonIndexedFilteredSelect)->Arg(1000)->Arg(100000);

void BM_UpdateRows(benchmark::State &state) {
    Parser parser;
    for (auto _ : state) {
        state.PauseTiming();
        QueryExecutor executor;
        (void)executor.execute(parser.parse("CREATE DATABASE bench;"));
        (void)executor.execute(parser.parse("CREATE TABLE Employees (id INT, salary DOUBLE);"));
        for (std::int64_t i = 0; i < state.range(0); ++i) {
            (void)executor.execute(Insert{"Employees", {{Value{i}, Value{100000.0}}}});
        }
        const auto query = parser.parse("UPDATE Employees SET salary = 125000.0 WHERE id > -1;");
        state.ResumeTiming();

        auto result = executor.execute(query);
        benchmark::DoNotOptimize(result);
    }
}

BENCHMARK(BM_UpdateRows)->Arg(1000)->Arg(10000);

void BM_DeleteRows(benchmark::State &state) {
    Parser parser;
    for (auto _ : state) {
        state.PauseTiming();
        QueryExecutor executor;
        (void)executor.execute(parser.parse("CREATE DATABASE bench;"));
        (void)executor.execute(parser.parse("CREATE TABLE Employees (id INT);"));
        for (std::int64_t i = 0; i < state.range(0); ++i) {
            (void)executor.execute(Insert{"Employees", {{Value{i}}}});
        }
        const auto query = parser.parse("DELETE FROM Employees WHERE id > -1;");
        state.ResumeTiming();

        auto result = executor.execute(query);
        benchmark::DoNotOptimize(result);
    }
}

BENCHMARK(BM_DeleteRows)->Arg(1000)->Arg(10000);

void BM_ConcurrentPointLookups(benchmark::State &state) {
    Parser parser;
    QueryExecutor executor;
    (void)executor.execute(parser.parse("CREATE DATABASE bench;"));
    (void)executor.execute(parser.parse("CREATE TABLE Employees (id INT, name STRING);"));
    (void)executor.execute(parser.parse("CREATE INDEX idx_id ON Employees(id);"));
    for (std::int64_t i = 0; i < 10000; ++i) {
        (void)executor.execute(Insert{"Employees", {{Value{i}, Value{std::string{"employee"}}}}});
    }
    const auto query = parser.parse("SELECT name FROM Employees WHERE id = 5000;");
    const auto workers = static_cast<int>(state.range(0));

    for (auto _ : state) {
        std::vector<std::future<QueryResult>> futures;
        futures.reserve(static_cast<std::size_t>(workers));
        for (int worker = 0; worker < workers; ++worker) {
            futures.push_back(std::async(std::launch::async,
                                         [&executor, &query] { return executor.execute(query); }));
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

} // namespace
} // namespace VertexDB
