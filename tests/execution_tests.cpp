#include "VertexDB/execution/query_executor.hpp"
#include "VertexDB/parser/parser.hpp"
#include "VertexDB/persistence/write_ahead_log.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <thread>

namespace VertexDB {

TEST(ExecutionTests, UpdatesAndDeletesByStableRowIds) {
    Parser parser;
    QueryExecutor executor;

    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(
        executor
            .execute(parser.parse("CREATE TABLE Employees (id INT, name STRING, salary DOUBLE);"))
            .success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "INSERT INTO Employees VALUES (1, \"Alice\", 120000.0), (2, \"Bob\", "
                        "90000.0), (3, \"Cara\", 110000.0);"))
                    .success);
    ASSERT_TRUE(executor.execute(parser.parse("CREATE INDEX idx_id ON Employees(id);")).success);

    ASSERT_TRUE(executor.execute(parser.parse("DELETE FROM Employees WHERE id = 2;")).success);
    ASSERT_TRUE(
        executor.execute(parser.parse("UPDATE Employees SET salary = 130000.0 WHERE id = 3;"))
            .success);

    auto remaining =
        executor.execute(parser.parse("SELECT id, salary FROM Employees ORDER BY id ASC;"));
    ASSERT_TRUE(remaining.success);
    ASSERT_EQ(remaining.rows.size(), 2U);
    EXPECT_EQ(remaining.rows[0][0], Value{static_cast<std::int64_t>(1)});
    EXPECT_EQ(remaining.rows[0][1], Value{120000.0});
    EXPECT_EQ(remaining.rows[1][0], Value{static_cast<std::int64_t>(3)});
    EXPECT_EQ(remaining.rows[1][1], Value{130000.0});

    ASSERT_TRUE(
        executor.execute(parser.parse("INSERT INTO Employees VALUES (4, \"Dana\", 95000.0);"))
            .success);
    auto afterInsert =
        executor.execute(parser.parse("SELECT id FROM Employees WHERE id = 4;"));
    ASSERT_TRUE(afterInsert.success);
    ASSERT_EQ(afterInsert.rows.size(), 1U);
    EXPECT_EQ(afterInsert.rows.front().front(), Value{static_cast<std::int64_t>(4)});
}

TEST(ExecutionTests, OrdersLimitsAndManagesTables) {
    Parser parser;
    QueryExecutor executor;

    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(
        executor
            .execute(parser.parse("CREATE TABLE Employees (id INT, name STRING, salary DOUBLE);"))
            .success);
    ASSERT_TRUE(
        executor.execute(parser.parse("INSERT INTO Employees VALUES (1, \"Alice\", 120000.0);"))
            .success);
    ASSERT_TRUE(
        executor.execute(parser.parse("INSERT INTO Employees VALUES (2, \"Bob\", 90000.0);"))
            .success);

    auto ordered =
        executor.execute(parser.parse("SELECT name FROM Employees ORDER BY salary ASC LIMIT 1;"));
    ASSERT_EQ(ordered.rows.size(), 1U);
    EXPECT_EQ(ordered.rows.front().front(), Value{std::string{"Bob"}});

    auto listed = executor.execute(parser.parse("LIST TABLES;"));
    ASSERT_EQ(listed.rows.size(), 1U);
    EXPECT_EQ(listed.rows.front().front(), Value{std::string{"Employees"}});

    EXPECT_TRUE(executor.execute(parser.parse("RENAME TABLE Employees TO Staff;")).success);
    EXPECT_TRUE(executor.execute(parser.parse("DROP TABLE Staff;")).success);
}

TEST(ExecutionTests, RollsBackTransactions) {
    Parser parser;
    QueryExecutor executor;

    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(executor.execute(parser.parse("CREATE TABLE Employees (id INT);")).success);
    ASSERT_TRUE(executor.execute(parser.parse("BEGIN;")).success);
    ASSERT_TRUE(executor.execute(parser.parse("INSERT INTO Employees VALUES (1);")).success);
    ASSERT_TRUE(executor.execute(parser.parse("ROLLBACK;")).success);

    auto result = executor.execute(parser.parse("SELECT * FROM Employees;"));
    EXPECT_TRUE(result.rows.empty());
}

TEST(ExecutionTests, TransactionalIndexedReadsUseMvccBoundary) {
    Parser parser;
    QueryExecutor executor;

    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(
        executor.execute(parser.parse("CREATE TABLE Employees (id INT, name STRING);")).success);
    ASSERT_TRUE(executor.execute(parser.parse("CREATE INDEX idx_id ON Employees(id);")).success);
    ASSERT_TRUE(
        executor.execute(parser.parse("INSERT INTO Employees VALUES (1, \"Alice\");")).success);

    ASSERT_TRUE(executor.execute(parser.parse("BEGIN;")).success);
    ASSERT_TRUE(
        executor.execute(parser.parse("UPDATE Employees SET name = \"Alicia\" WHERE id = 1;"))
            .success);

    auto inTransaction = executor.execute(parser.parse("SELECT name FROM Employees WHERE id = 1;"));
    ASSERT_EQ(inTransaction.rows.size(), 1U);
    EXPECT_EQ(inTransaction.rows.front().front(), Value{std::string{"Alicia"}});

    ASSERT_TRUE(executor.execute(parser.parse("ROLLBACK;")).success);
    auto afterRollback = executor.execute(parser.parse("SELECT name FROM Employees WHERE id = 1;"));
    ASSERT_EQ(afterRollback.rows.size(), 1U);
    EXPECT_EQ(afterRollback.rows.front().front(), Value{std::string{"Alice"}});
}

TEST(ExecutionTests, SavesAndLoadsDatabase) {
    const auto root = std::filesystem::temp_directory_path() /
                      ("VertexDB_test_" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Parser parser;

    {
        QueryExecutor executor{root};
        ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
        ASSERT_TRUE(executor.execute(parser.parse("CREATE TABLE Employees (id INT, name STRING);"))
                        .success);
        ASSERT_TRUE(
            executor.execute(parser.parse("CREATE INDEX idx_id ON Employees(id);")).success);
        ASSERT_TRUE(
            executor.execute(parser.parse("INSERT INTO Employees VALUES (1, \"Alice\");")).success);
        ASSERT_TRUE(executor.execute(parser.parse("SAVE DATABASE;")).success);
    }

    QueryExecutor loaded{root};
    ASSERT_TRUE(loaded.execute(parser.parse("LOAD DATABASE company;")).success);
    auto result = loaded.execute(parser.parse("SELECT name FROM Employees WHERE id = 1;"));
    ASSERT_EQ(result.rows.size(), 1U);
    EXPECT_EQ(result.rows.front().front(), Value{std::string{"Alice"}});

    std::filesystem::remove_all(root);
}

TEST(ExecutionTests, SaveLoadPreservesSparseRowIdsAndFreeListReuse) {
    const auto root =
        std::filesystem::temp_directory_path() /
        ("VertexDB_sparse_ids_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Parser parser;

    RowId keptId = 0;
    RowId deletedId = 0;
    {
        QueryExecutor executor{root};
        ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
        ASSERT_TRUE(executor.execute(parser.parse("CREATE TABLE Employees (id INT, name STRING);"))
                        .success);
        ASSERT_TRUE(
            executor.execute(parser.parse("CREATE INDEX idx_id ON Employees(id);")).success);
        ASSERT_TRUE(executor
                        .execute(parser.parse(
                            "INSERT INTO Employees VALUES (1, \"Alice\"), (2, \"Bob\"), (3, "
                            "\"Cara\");"))
                        .success);

        auto *table = executor.currentDatabase()->table("Employees").get();
        ASSERT_NE(table, nullptr);
        const auto beforeDelete = table->liveEntries();
        ASSERT_EQ(beforeDelete.size(), 3U);
        deletedId = beforeDelete[1].first;
        keptId = beforeDelete[2].first;

        ASSERT_TRUE(executor.execute(parser.parse("DELETE FROM Employees WHERE id = 2;")).success);
        ASSERT_EQ(table->capacity(), 3U);
        ASSERT_EQ(table->rowCount(), 2U);
        ASSERT_EQ(table->freeList(), std::vector<RowId>{deletedId});
        ASSERT_TRUE(executor.execute(parser.parse("SAVE DATABASE;")).success);
    }

    QueryExecutor loaded{root};
    ASSERT_TRUE(loaded.execute(parser.parse("LOAD DATABASE company;")).success);
    auto *table = loaded.currentDatabase()->table("Employees").get();
    ASSERT_NE(table, nullptr);
    EXPECT_EQ(table->capacity(), 3U);
    EXPECT_EQ(table->rowCount(), 2U);
    EXPECT_EQ(table->freeList(), std::vector<RowId>{deletedId});
    ASSERT_TRUE(table->hasIndex("id"));
    ASSERT_EQ(table->listIndexes(), (std::vector<std::string>{"idx_id"}));
    {
        const auto definitions = table->indexDefinitions();
        ASSERT_EQ(definitions.size(), 1U);
        EXPECT_EQ(definitions.front().name, "idx_id");
        EXPECT_EQ(definitions.front().column, "id");
        EXPECT_FALSE(definitions.front().expression.has_value());
    }
    EXPECT_EQ(table->indexedLookup("id", Value{static_cast<std::int64_t>(3)})
                  .value_or(std::vector<RowId>{}),
              std::vector<RowId>{keptId});

    ASSERT_TRUE(
        loaded.execute(parser.parse("INSERT INTO Employees VALUES (4, \"Dana\");")).success);
    EXPECT_EQ(table->indexedLookup("id", Value{static_cast<std::int64_t>(4)})
                  .value_or(std::vector<RowId>{}),
              std::vector<RowId>{deletedId});

    std::filesystem::remove_all(root);
}

TEST(ExecutionTests, FailedMetadataOperationsDoNotPolluteWal) {
    const auto root = std::filesystem::temp_directory_path() /
                      ("VertexDB_failed_wal_test_" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Parser parser;
    QueryExecutor executor{root};

    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(executor.execute(parser.parse("CREATE TABLE Employees (id INT);")).success);
    EXPECT_FALSE(executor.execute(parser.parse("CREATE TABLE Employees (id INT);")).success);
    EXPECT_FALSE(executor.execute(parser.parse("DROP TABLE Missing;")).success);

    WriteAheadLog wal{root / "VertexDB.wal"};
    const auto records = wal.readAll();
    ASSERT_EQ(records.size(), 2U);
    EXPECT_EQ(records[0].operation, WalOperation::CreateDatabase);
    EXPECT_EQ(records[1].operation, WalOperation::CreateTable);

    std::filesystem::remove_all(root);
}

TEST(ExecutionTests, FailedInsertDoesNotPolluteWal) {
    const auto root = std::filesystem::temp_directory_path() /
                      ("VertexDB_failed_insert_wal_test_" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Parser parser;
    QueryExecutor executor{root};

    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(executor.execute(parser.parse("CREATE TABLE Employees (id INT);")).success);
    EXPECT_THROW(
        (void)executor.execute(parser.parse("INSERT INTO Employees VALUES (1, \"extra\");")),
        std::invalid_argument);

    WriteAheadLog wal{root / "VertexDB.wal"};
    const auto records = wal.readAll();
    ASSERT_EQ(records.size(), 2U);
    EXPECT_EQ(records[0].operation, WalOperation::CreateDatabase);
    EXPECT_EQ(records[1].operation, WalOperation::CreateTable);

    std::filesystem::remove_all(root);
}

TEST(ExecutionTests, ExecutesPreparedStatementsWithParameters) {
    const auto root = std::filesystem::temp_directory_path() /
                      ("VertexDB_prepared_test_" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Parser parser;
    QueryExecutor executor{root};

    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(
        executor.execute(parser.parse("CREATE TABLE Employees (id INT, name STRING);")).success);
    ASSERT_TRUE(
        executor.execute(parser.parse("INSERT INTO Employees VALUES (1, \"Alice\");")).success);
    ASSERT_TRUE(
        executor
            .execute(parser.parse("PREPARE by_id AS \"SELECT name FROM Employees WHERE id = ?;\";"))
            .success);

    auto result = executor.execute(parser.parse("EXECUTE by_id VALUES (1);"));
    ASSERT_EQ(result.rows.size(), 1U);
    EXPECT_EQ(result.rows.front().front(), Value{std::string{"Alice"}});

    const auto ast = executor.preparedAst("by_id");
    ASSERT_TRUE(ast.has_value());
    ASSERT_TRUE(std::holds_alternative<Select>(*ast));
    EXPECT_TRUE(std::get<Select>(*ast).where->value.isParameter());

    std::filesystem::remove_all(root);
}

TEST(ExecutionTests, ExecutesAggregatesGroupByAndMultiJoin) {
    const auto root = std::filesystem::temp_directory_path() /
                      ("VertexDB_agg_join_test_" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Parser parser;
    QueryExecutor executor{root};

    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "CREATE TABLE Employees (id INT, name STRING, salary DOUBLE, dept_id INT);"))
                    .success);
    ASSERT_TRUE(
        executor.execute(parser.parse("CREATE TABLE Departments (id INT, office_id INT);")).success);
    ASSERT_TRUE(
        executor.execute(parser.parse("CREATE TABLE Offices (id INT, city STRING);")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "INSERT INTO Employees VALUES (1, \"Alice\", 120000.0, 10), "
                        "(2, \"Bob\", 90000.0, 20), (3, \"Cara\", 110000.0, 10);"))
                    .success);
    ASSERT_TRUE(
        executor.execute(parser.parse("INSERT INTO Departments VALUES (10, 100), (20, 200);"))
            .success);
    ASSERT_TRUE(executor
                    .execute(parser.parse("INSERT INTO Offices VALUES (100, \"SF\"), (200, \"NY\");"))
                    .success);

    auto aggregates =
        executor.execute(parser.parse("SELECT COUNT(name), MIN(salary), MAX(salary) FROM Employees;"));
    ASSERT_TRUE(aggregates.success);
    ASSERT_EQ(aggregates.rows.size(), 1U);
    EXPECT_EQ(aggregates.rows[0][0], Value{3});
    EXPECT_EQ(aggregates.rows[0][1], Value{90000.0});
    EXPECT_EQ(aggregates.rows[0][2], Value{120000.0});

    auto multiJoin = executor.execute(parser.parse(
        "SELECT Employees.name, Offices.city FROM Employees "
        "JOIN Departments ON Employees.dept_id = Departments.id "
        "JOIN Offices ON Departments.office_id = Offices.id "
        "ORDER BY Employees.name;"));
    ASSERT_TRUE(multiJoin.success);
    ASSERT_EQ(multiJoin.rows.size(), 3U);
    EXPECT_EQ(multiJoin.rows[0][0], Value{std::string{"Alice"}});
    EXPECT_EQ(multiJoin.rows[0][1], Value{std::string{"SF"}});

    std::filesystem::remove_all(root);
}

TEST(ExecutionTests, ExecutesMultiRowInsertCompoundPredicatesAndNullPersistence) {
    const auto root = std::filesystem::temp_directory_path() /
                      ("VertexDB_sql_surface_test_" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Parser parser;

    {
        QueryExecutor executor{root};
        ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
        ASSERT_TRUE(executor
                        .execute(parser.parse(
                            "CREATE TABLE People (id INT, name STRING, nickname STRING NULL);"))
                        .success);
        ASSERT_TRUE(executor
                        .execute(parser.parse("INSERT INTO People VALUES (1, \"Alice\", NULL), "
                                              "(2, \"Bob\", \"Bobby\"), (3, \"Cara\", NULL);"))
                        .success);

        auto result = executor.execute(parser.parse(
            "SELECT name FROM People WHERE id = 1 OR (id > 2 AND name = \"Cara\") ORDER BY name "
            "ASC;"));
        ASSERT_EQ(result.rows.size(), 2U);
        EXPECT_EQ(result.rows[0].front(), Value{std::string{"Alice"}});
        EXPECT_EQ(result.rows[1].front(), Value{std::string{"Cara"}});
        ASSERT_TRUE(executor.execute(parser.parse("SAVE DATABASE;")).success);
    }

    QueryExecutor recovered{root};
    auto nullResult =
        recovered.execute(parser.parse("SELECT nickname FROM People WHERE name = \"Alice\";"));
    ASSERT_EQ(nullResult.rows.size(), 1U);
    EXPECT_TRUE(nullResult.rows.front().front().isNull());

    std::filesystem::remove_all(root);
}

TEST(ExecutionTests, ExecutesHashJoin) {
    const auto root = std::filesystem::temp_directory_path() /
                      ("VertexDB_join_test_" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Parser parser;
    QueryExecutor executor{root};

    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(
        executor.execute(parser.parse("CREATE TABLE Employees (id INT, name STRING, dept_id INT);"))
            .success);
    ASSERT_TRUE(
        executor.execute(parser.parse("CREATE TABLE Departments (id INT, dept STRING);")).success);
    ASSERT_TRUE(
        executor.execute(parser.parse("INSERT INTO Employees VALUES (1, \"Alice\", 10);")).success);
    ASSERT_TRUE(
        executor.execute(parser.parse("INSERT INTO Departments VALUES (10, \"Engineering\");"))
            .success);

    auto result = executor.execute(
        parser.parse("SELECT * FROM Employees JOIN Departments ON dept_id = id LIMIT 1;"));
    ASSERT_EQ(result.rows.size(), 1U);
    ASSERT_EQ(result.columns.size(), 5U);
    EXPECT_EQ(result.columns[0], "Employees.id");
    EXPECT_EQ(result.columns[4], "Departments.dept");
    EXPECT_EQ(result.rows.front()[1], Value{std::string{"Alice"}});
    EXPECT_EQ(result.rows.front()[4], Value{std::string{"Engineering"}});

    std::filesystem::remove_all(root);
}

TEST(ExecutionTests, ExecutesProjectedQualifiedJoinWithFilteringAndOrdering) {
    const auto root = std::filesystem::temp_directory_path() /
                      ("VertexDB_projected_join_test_" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Parser parser;
    QueryExecutor executor{root};

    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(
        executor.execute(parser.parse("CREATE TABLE Employees (id INT, name STRING, dept_id INT);"))
            .success);
    ASSERT_TRUE(
        executor.execute(parser.parse("CREATE TABLE Departments (id INT, dept STRING);")).success);
    ASSERT_TRUE(
        executor.execute(parser.parse("INSERT INTO Employees VALUES (1, \"Alice\", 10);")).success);
    ASSERT_TRUE(
        executor.execute(parser.parse("INSERT INTO Employees VALUES (2, \"Bob\", 20);")).success);
    ASSERT_TRUE(
        executor.execute(parser.parse("INSERT INTO Departments VALUES (10, \"Engineering\");"))
            .success);
    ASSERT_TRUE(
        executor.execute(parser.parse("INSERT INTO Departments VALUES (20, \"Sales\");")).success);

    auto result = executor.execute(
        parser.parse("SELECT Employees.name, Departments.dept FROM Employees JOIN Departments ON "
                     "Employees.dept_id = Departments.id WHERE Departments.dept > \"A\" ORDER BY "
                     "Employees.name DESC LIMIT 1;"));
    ASSERT_EQ(result.columns, (std::vector<std::string>{"Employees.name", "Departments.dept"}));
    ASSERT_EQ(result.rows.size(), 1U);
    EXPECT_EQ(result.rows.front()[0], Value{std::string{"Bob"}});
    EXPECT_EQ(result.rows.front()[1], Value{std::string{"Sales"}});

    std::filesystem::remove_all(root);
}

TEST(ExecutionTests, AutomaticallyLoadsSavedDatabaseOnStartup) {
    const auto root = std::filesystem::temp_directory_path() /
                      ("VertexDB_recovery_test_" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Parser parser;

    {
        QueryExecutor executor{root};
        ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
        ASSERT_TRUE(executor.execute(parser.parse("CREATE TABLE Employees (id INT);")).success);
        ASSERT_TRUE(executor.execute(parser.parse("INSERT INTO Employees VALUES (1);")).success);
        ASSERT_TRUE(executor.execute(parser.parse("SAVE DATABASE;")).success);
    }

    QueryExecutor recovered{root};
    auto result = recovered.execute(parser.parse("SELECT * FROM Employees;"));
    ASSERT_EQ(result.rows.size(), 1U);
    EXPECT_EQ(result.rows.front().front(), Value{1});

    std::filesystem::remove_all(root);
}

TEST(ExecutionTests, ReplaysWalChangesAfterLatestSavedSnapshot) {
    const auto root = std::filesystem::temp_directory_path() /
                      ("VertexDB_wal_recovery_after_save_test_" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Parser parser;

    {
        QueryExecutor executor{root};
        ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
        ASSERT_TRUE(executor.execute(parser.parse("CREATE TABLE Employees (id INT);")).success);
        ASSERT_TRUE(executor.execute(parser.parse("INSERT INTO Employees VALUES (1);")).success);
        ASSERT_TRUE(executor.execute(parser.parse("SAVE DATABASE;")).success);
        ASSERT_TRUE(executor.execute(parser.parse("INSERT INTO Employees VALUES (2);")).success);
    }

    QueryExecutor recovered{root};
    auto result = recovered.execute(parser.parse("SELECT * FROM Employees ORDER BY id ASC;"));
    ASSERT_EQ(result.rows.size(), 2U);
    EXPECT_EQ(result.rows[0].front(), Value{1});
    EXPECT_EQ(result.rows[1].front(), Value{2});

    std::filesystem::remove_all(root);
}

TEST(ExecutionTests, ReplaysWalWhenNoSnapshotExists) {
    const auto root = std::filesystem::temp_directory_path() /
                      ("VertexDB_wal_only_recovery_test_" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Parser parser;

    {
        QueryExecutor executor{root};
        ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
        ASSERT_TRUE(executor.execute(parser.parse("CREATE TABLE Employees (id INT, name STRING);"))
                        .success);
        ASSERT_TRUE(
            executor.execute(parser.parse("INSERT INTO Employees VALUES (1, \"Alice\");")).success);
    }

    QueryExecutor recovered{root};
    auto result = recovered.execute(parser.parse("SELECT name FROM Employees WHERE id = 1;"));
    ASSERT_EQ(result.rows.size(), 1U);
    EXPECT_EQ(result.rows.front().front(), Value{std::string{"Alice"}});

    std::filesystem::remove_all(root);
}

TEST(ExecutionTests, SupportsConcurrentExecutorClients) {
    const auto root = std::filesystem::temp_directory_path() /
                      ("VertexDB_executor_concurrency_test_" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Parser parser;
    QueryExecutor executor{root};
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(executor.execute(parser.parse("CREATE TABLE Events (id INT);")).success);

    constexpr int threadCount = 4;
    constexpr int insertsPerThread = 100;
    std::vector<std::thread> threads;
    for (int thread = 0; thread < threadCount; ++thread) {
        threads.emplace_back([thread, &executor] {
            for (int i = 0; i < insertsPerThread; ++i) {
                const int id = thread * insertsPerThread + i;
                (void)executor.execute(Insert{"Events", {{Value{id}}}});
            }
        });
    }
    for (auto &thread : threads) {
        thread.join();
    }

    auto result = executor.execute(parser.parse("SELECT * FROM Events;"));
    EXPECT_EQ(result.rows.size(), static_cast<std::size_t>(threadCount * insertsPerThread));

    std::filesystem::remove_all(root);
}

} // namespace VertexDB
