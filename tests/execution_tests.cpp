#include "VertexDB/execution/query_executor.hpp"
#include "VertexDB/parser/parser.hpp"
#include "VertexDB/persistence/write_ahead_log.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <thread>

namespace VertexDB {

TEST(ExecutionTests, InsertsAndSelectsRows) {
    Parser parser;
    QueryExecutor executor;

    EXPECT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    EXPECT_TRUE(
        executor
            .execute(parser.parse("CREATE TABLE Employees (id INT, name STRING, salary DOUBLE);"))
            .success);
    EXPECT_TRUE(
        executor.execute(parser.parse("INSERT INTO Employees VALUES (1, \"Alice\", 120000.0);"))
            .success);

    auto result = executor.execute(
        parser.parse("SELECT name FROM Employees WHERE salary > 100000.0 LIMIT 1;"));
    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.rows.size(), 1U);
    EXPECT_EQ(result.rows.front().front(), Value{std::string{"Alice"}});
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
