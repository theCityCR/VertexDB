#include "VertexDB/execution/query_executor.hpp"
#include "VertexDB/parser/parser.hpp"
#include "VertexDB/persistence/write_ahead_log.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <string>

namespace VertexDB {
namespace {

std::filesystem::path testRoot(std::string_view name) {
    return std::filesystem::temp_directory_path() /
           (std::string{"VertexDB_regression_"} + std::string{name} + "_" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
}

} // namespace

TEST(RegressionTests, InvalidMultiRowInsertIsAtomicAndDoesNotWriteWalRecord) {
    const auto root = testRoot("atomic_multi_insert");
    Parser parser;
    QueryExecutor executor{root};

    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(
        executor.execute(parser.parse("CREATE TABLE Employees (id INT, name STRING);")).success);

    EXPECT_THROW((void)executor.execute(
                     parser.parse("INSERT INTO Employees VALUES (1, \"Alice\"), (2, NULL);")),
                 std::invalid_argument);

    auto result = executor.execute(parser.parse("SELECT * FROM Employees;"));
    EXPECT_TRUE(result.rows.empty());

    const auto records = WriteAheadLog{root / "VertexDB.wal"}.readAll();
    ASSERT_EQ(records.size(), 2U);
    EXPECT_EQ(records[0].operation, WalOperation::CreateDatabase);
    EXPECT_EQ(records[1].operation, WalOperation::CreateTable);

    std::filesystem::remove_all(root);
}

TEST(RegressionTests, SaveCheckpointsWalBeforePostSaveMutations) {
    const auto root = testRoot("wal_checkpoint");
    Parser parser;

    QueryExecutor executor{root};
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(executor.execute(parser.parse("CREATE TABLE Events (id INT);")).success);
    ASSERT_TRUE(executor.execute(parser.parse("INSERT INTO Events VALUES (1);")).success);
    ASSERT_TRUE(executor.execute(parser.parse("SAVE DATABASE;")).success);
    ASSERT_TRUE(executor.execute(parser.parse("INSERT INTO Events VALUES (2);")).success);

    const auto records = WriteAheadLog{root / "VertexDB.wal"}.readAll();
    ASSERT_EQ(records.size(), 1U);
    EXPECT_EQ(records.front().operation, WalOperation::Insert);

    QueryExecutor recovered{root};
    auto result = recovered.execute(parser.parse("SELECT * FROM Events ORDER BY id ASC;"));
    ASSERT_EQ(result.rows.size(), 2U);
    EXPECT_EQ(result.rows[0].front(), Value{1});
    EXPECT_EQ(result.rows[1].front(), Value{2});

    std::filesystem::remove_all(root);
}

TEST(RegressionTests, AmbiguousUnqualifiedJoinProjectionIsRejected) {
    const auto root = testRoot("ambiguous_join");
    Parser parser;
    QueryExecutor executor{root};

    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(
        executor.execute(parser.parse("CREATE TABLE Employees (id INT, dept_id INT);")).success);
    ASSERT_TRUE(
        executor.execute(parser.parse("CREATE TABLE Departments (id INT, name STRING);")).success);
    ASSERT_TRUE(executor.execute(parser.parse("INSERT INTO Employees VALUES (1, 10);")).success);
    ASSERT_TRUE(
        executor.execute(parser.parse("INSERT INTO Departments VALUES (10, \"Engineering\");"))
            .success);

    EXPECT_THROW((void)executor.execute(
                     parser.parse("SELECT id FROM Employees JOIN Departments ON dept_id = id;")),
                 std::runtime_error);

    std::filesystem::remove_all(root);
}

TEST(RegressionTests, PreparedStringParametersEscapeQuotesAndBackslashes) {
    const auto root = testRoot("prepared_escaping");
    Parser parser;
    QueryExecutor executor{root};
    const std::string expected = "A \"quoted\" \\ name";

    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(executor.execute(parser.parse("CREATE TABLE Names (value STRING);")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse("PREPARE insert_name AS "
                                          "\"INSERT INTO Names VALUES (?);\";"))
                    .success);
    ASSERT_TRUE(
        executor.execute(parser.parse(R"(EXECUTE insert_name VALUES ("A \"quoted\" \\ name");)"))
            .success);

    auto result = executor.execute(parser.parse("SELECT value FROM Names;"));
    ASSERT_EQ(result.rows.size(), 1U);
    EXPECT_EQ(result.rows.front().front(), Value{expected});

    std::filesystem::remove_all(root);
}

TEST(RegressionTests, NullableIndexedColumnCanFindNullValues) {
    const auto root = testRoot("nullable_index");
    Parser parser;
    QueryExecutor executor{root};

    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(
        executor.execute(parser.parse("CREATE TABLE People (id INT, nickname STRING NULL);"))
            .success);
    ASSERT_TRUE(
        executor.execute(parser.parse("CREATE INDEX idx_nickname ON People(nickname);")).success);
    ASSERT_TRUE(executor.execute(parser.parse("INSERT INTO People VALUES (1, NULL);")).success);

    auto result = executor.execute(parser.parse("SELECT id FROM People WHERE nickname = NULL;"));
    ASSERT_EQ(result.rows.size(), 1U);
    EXPECT_EQ(result.rows.front().front(), Value{1});

    std::filesystem::remove_all(root);
}

TEST(RegressionTests, NullableUpdatesAcceptNullAndStrictUpdatesRejectNull) {
    const auto root = testRoot("nullable_update");
    Parser parser;
    QueryExecutor executor{root};

    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(
        executor.execute(parser.parse("CREATE TABLE People (id INT, nickname STRING NULL);"))
            .success);
    ASSERT_TRUE(executor.execute(parser.parse("INSERT INTO People VALUES (1, \"Al\");")).success);
    ASSERT_TRUE(
        executor.execute(parser.parse("UPDATE People SET nickname = NULL WHERE id = 1;")).success);

    auto result = executor.execute(parser.parse("SELECT nickname FROM People WHERE id = 1;"));
    ASSERT_EQ(result.rows.size(), 1U);
    EXPECT_TRUE(result.rows.front().front().isNull());

    EXPECT_THROW((void)executor.execute(parser.parse("UPDATE People SET id = NULL WHERE id = 1;")),
                 std::invalid_argument);

    std::filesystem::remove_all(root);
}

} // namespace VertexDB
