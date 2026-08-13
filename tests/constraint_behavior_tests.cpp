#include "test_support.hpp"

#include "VertexDB/execution/query_executor.hpp"
#include "VertexDB/parser/parser.hpp"
#include "VertexDB/parser/predicate.hpp"
#include "VertexDB/planner/query_planner.hpp"
#include "VertexDB/storage/table.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <stdexcept>
#include <string>

namespace VertexDB {

TEST(ConstraintBehaviorTests, ParsesPrimaryKeyAndUniqueColumns) {
    Parser parser;
    auto query = parser.parse(
        "CREATE TABLE Accounts (id INT PRIMARY KEY, email STRING UNIQUE, note STRING NULL);");
    ASSERT_TRUE(std::holds_alternative<CreateTable>(query));
    const auto &table = std::get<CreateTable>(query);
    ASSERT_EQ(table.columns.size(), 3U);
    EXPECT_TRUE(table.columns[0].primaryKey);
    EXPECT_TRUE(table.columns[0].unique);
    EXPECT_FALSE(table.columns[0].nullable);
    EXPECT_TRUE(table.columns[1].unique);
    EXPECT_FALSE(table.columns[1].primaryKey);
    EXPECT_FALSE(table.columns[1].nullable);
    EXPECT_TRUE(table.columns[2].nullable);
    EXPECT_FALSE(table.columns[2].unique);
}

TEST(ConstraintBehaviorTests, RejectsPrimaryKeyNullAndMultiplePrimaryKeys) {
    Parser parser;
    EXPECT_THROW((void)parser.parse("CREATE TABLE T (id INT NULL PRIMARY KEY);"),
                 std::runtime_error);
    EXPECT_THROW((void)parser.parse("CREATE TABLE T (id INT PRIMARY KEY, other INT PRIMARY KEY);"),
                 std::runtime_error);
}

TEST(ConstraintBehaviorTests, PrimaryKeyRejectsDuplicateInsert) {
    Parser parser;
    auto executor = makeTempExecutor("vertexdb-constraint-", "pk-dup-insert");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(
        executor.execute(parser.parse("CREATE TABLE Accounts (id INT PRIMARY KEY, name STRING);"))
            .success);
    ASSERT_TRUE(
        executor.execute(parser.parse("INSERT INTO Accounts VALUES (1, \"Ada\");")).success);
    EXPECT_THROW((void)executor.execute(parser.parse("INSERT INTO Accounts VALUES (1, \"Bob\");")),
                 std::invalid_argument);
    const auto rows = executor.execute(parser.parse("SELECT name FROM Accounts;"));
    ASSERT_TRUE(rows.success);
    ASSERT_EQ(rows.rows.size(), 1U);
    EXPECT_EQ(rows.rows[0][0], Value{std::string{"Ada"}});
}

TEST(ConstraintBehaviorTests, UniqueConstraintRejectsDuplicateUpdate) {
    Parser parser;
    auto executor = makeTempExecutor("vertexdb-constraint-", "uq-dup-update");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "CREATE TABLE Accounts (id INT PRIMARY KEY, email STRING UNIQUE);"))
                    .success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "INSERT INTO Accounts VALUES (1, \"a@x\"), (2, \"b@x\");"))
                    .success);
    EXPECT_THROW(
        (void)executor.execute(parser.parse("UPDATE Accounts SET email = \"a@x\" WHERE id = 2;")),
        std::invalid_argument);
    const auto rows =
        executor.execute(parser.parse("SELECT email FROM Accounts WHERE id = 2;"));
    ASSERT_TRUE(rows.success);
    ASSERT_EQ(rows.rows.size(), 1U);
    EXPECT_EQ(rows.rows[0][0], Value{std::string{"b@x"}});
}

TEST(ConstraintBehaviorTests, MultiRowInsertRefusesDuplicateWithoutPartialWrite) {
    Parser parser;
    auto executor = makeTempExecutor("vertexdb-constraint-", "pk-batch-dup");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(
        executor.execute(parser.parse("CREATE TABLE Accounts (id INT PRIMARY KEY);")).success);
    EXPECT_THROW(
        (void)executor.execute(parser.parse("INSERT INTO Accounts VALUES (1), (1);")),
        std::invalid_argument);
    const auto rows = executor.execute(parser.parse("SELECT id FROM Accounts;"));
    ASSERT_TRUE(rows.success);
    EXPECT_TRUE(rows.rows.empty());
}

TEST(ConstraintBehaviorTests, UniqueAllowsMultipleNulls) {
    Parser parser;
    auto executor = makeTempExecutor("vertexdb-constraint-", "uq-nulls");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "CREATE TABLE Accounts (id INT PRIMARY KEY, email STRING NULL UNIQUE);"))
                    .success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "INSERT INTO Accounts VALUES (1, NULL), (2, NULL);"))
                    .success);
    const auto rows = executor.execute(parser.parse("SELECT id FROM Accounts ORDER BY id;"));
    ASSERT_TRUE(rows.success);
    ASSERT_EQ(rows.rows.size(), 2U);
}

TEST(ConstraintBehaviorTests, PrimaryKeyRejectsNullInsert) {
    Parser parser;
    auto executor = makeTempExecutor("vertexdb-constraint-", "pk-null");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(
        executor.execute(parser.parse("CREATE TABLE Accounts (id INT PRIMARY KEY);")).success);
    EXPECT_THROW((void)executor.execute(parser.parse("INSERT INTO Accounts VALUES (NULL);")),
                 std::invalid_argument);
}

TEST(ConstraintBehaviorTests, PrimaryKeyAutoIndexUsedForHashEq) {
    Parser parser;
    auto executor = makeTempExecutor("vertexdb-constraint-", "pk-hash-eq");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(
        executor.execute(parser.parse("CREATE TABLE Accounts (id INT PRIMARY KEY, name STRING);"))
            .success);
    ASSERT_TRUE(
        executor.execute(parser.parse("INSERT INTO Accounts VALUES (1, \"Ada\");")).success);

    auto database = executor.currentDatabase();
    ASSERT_NE(database, nullptr);
    auto table = database->table("Accounts");
    ASSERT_NE(table, nullptr);
    EXPECT_TRUE(table->hasIndex("id"));

    Select query{"Accounts",
                 {},
                 {SelectExpr::makeColumn("name")},
                 makeComparison("id", ComparisonOperator::Equal, Value{static_cast<std::int64_t>(1)}),
                 {},
                 {}};
    const auto plan = QueryPlanner{}.planSelect(query, *table);
    EXPECT_EQ(plan.accessPath(), AccessPath::HashEq);
}

TEST(ConstraintBehaviorTests, CannotDropConstraintIndex) {
    Parser parser;
    auto executor = makeTempExecutor("vertexdb-constraint-", "pk-drop-index");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(
        executor.execute(parser.parse("CREATE TABLE Accounts (id INT PRIMARY KEY);")).success);
    const auto dropped = executor.execute(parser.parse("DROP INDEX __pk_id ON Accounts;"));
    EXPECT_FALSE(dropped.success);
    auto table = executor.currentDatabase()->table("Accounts");
    ASSERT_NE(table, nullptr);
    EXPECT_TRUE(table->hasIndex("id"));
}

TEST(ConstraintBehaviorTests, SaveLoadPreservesPrimaryKey) {
    Parser parser;
    auto executor = makeTempExecutor("vertexdb-constraint-", "pk-save-load");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "CREATE TABLE Accounts (id INT PRIMARY KEY, email STRING UNIQUE);"))
                    .success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "INSERT INTO Accounts VALUES (1, \"a@x\");"))
                    .success);
    ASSERT_TRUE(executor.execute(parser.parse("SAVE DATABASE;")).success);
    ASSERT_TRUE(executor.execute(parser.parse("LOAD DATABASE company;")).success);

    auto table = executor.currentDatabase()->table("Accounts");
    ASSERT_NE(table, nullptr);
    ASSERT_EQ(table->schema().size(), 2U);
    EXPECT_TRUE(table->schema()[0].primaryKey);
    EXPECT_TRUE(table->schema()[0].unique);
    EXPECT_TRUE(table->schema()[1].unique);
    EXPECT_TRUE(table->hasIndex("id"));
    EXPECT_TRUE(table->hasIndex("email"));

    EXPECT_THROW((void)executor.execute(parser.parse("INSERT INTO Accounts VALUES (1, \"b@x\");")),
                 std::invalid_argument);
    EXPECT_THROW(
        (void)executor.execute(parser.parse("INSERT INTO Accounts VALUES (2, \"a@x\");")),
        std::invalid_argument);
}

} // namespace VertexDB
