#include "test_support.hpp"

#include "VertexDB/execution/foreign_key_eval.hpp"
#include "VertexDB/execution/query_executor.hpp"
#include "VertexDB/execution/sql_literal.hpp"
#include "VertexDB/parser/parser.hpp"
#include "VertexDB/parser/predicate.hpp"
#include "VertexDB/planner/query_planner.hpp"
#include "VertexDB/storage/check_eval.hpp"
#include "VertexDB/storage/database.hpp"
#include "VertexDB/storage/foreign_key.hpp"
#include "VertexDB/storage/table.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

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
    EXPECT_THROW((void)parser.parse(
                     "CREATE TABLE T (a INT, b INT, PRIMARY KEY (a, b), PRIMARY KEY (a));"),
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

TEST(ConstraintBehaviorTests, UniqueConstraintRejectsDuplicateInsert) {
    Parser parser;
    auto executor = makeTempExecutor("vertexdb-constraint-", "uq-dup-insert");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "CREATE TABLE Accounts (id INT PRIMARY KEY, email STRING UNIQUE);"))
                    .success);
    ASSERT_TRUE(
        executor.execute(parser.parse("INSERT INTO Accounts VALUES (1, \"a@x\");")).success);
    EXPECT_THROW(
        (void)executor.execute(parser.parse("INSERT INTO Accounts VALUES (2, \"a@x\");")),
        std::invalid_argument);
    const auto rows = executor.execute(parser.parse("SELECT id, email FROM Accounts;"));
    ASSERT_TRUE(rows.success);
    ASSERT_EQ(rows.rows.size(), 1U);
    EXPECT_EQ(rows.rows[0][0], Value{static_cast<std::int64_t>(1)});
    EXPECT_EQ(rows.rows[0][1], Value{std::string{"a@x"}});
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
    try {
        (void)executor.execute(parser.parse("INSERT INTO Accounts VALUES (NULL);"));
        FAIL() << "expected NOT NULL rejection";
    } catch (const std::invalid_argument &ex) {
        EXPECT_NE(std::string{ex.what()}.find("NOT NULL constraint violation on column id"),
                  std::string::npos);
    }
}

TEST(ConstraintBehaviorTests, ParsesExplicitNotNull) {
    Parser parser;
    auto query = parser.parse("CREATE TABLE People (id INT NOT NULL, nickname STRING NULL);");
    ASSERT_TRUE(std::holds_alternative<CreateTable>(query));
    const auto &table = std::get<CreateTable>(query);
    ASSERT_EQ(table.columns.size(), 2U);
    EXPECT_FALSE(table.columns[0].nullable);
    EXPECT_TRUE(table.columns[1].nullable);
}

TEST(ConstraintBehaviorTests, NotNullRejectsNullInsert) {
    Parser parser;
    auto executor = makeTempExecutor("vertexdb-constraint-", "not-null-insert");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "CREATE TABLE People (id INT NOT NULL, name STRING NOT NULL);"))
                    .success);
    try {
        (void)executor.execute(parser.parse("INSERT INTO People VALUES (1, NULL);"));
        FAIL() << "expected NOT NULL rejection";
    } catch (const std::invalid_argument &ex) {
        EXPECT_NE(std::string{ex.what()}.find("NOT NULL constraint violation on column name"),
                  std::string::npos);
    }
    const auto rows = executor.execute(parser.parse("SELECT id FROM People;"));
    ASSERT_TRUE(rows.success);
    EXPECT_TRUE(rows.rows.empty());
}

TEST(ConstraintBehaviorTests, NotNullRejectsNullUpdate) {
    Parser parser;
    auto executor = makeTempExecutor("vertexdb-constraint-", "not-null-update");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "CREATE TABLE People (id INT NOT NULL, name STRING NOT NULL);"))
                    .success);
    ASSERT_TRUE(
        executor.execute(parser.parse("INSERT INTO People VALUES (1, \"Ada\");")).success);
    try {
        (void)executor.execute(parser.parse("UPDATE People SET name = NULL WHERE id = 1;"));
        FAIL() << "expected NOT NULL rejection";
    } catch (const std::invalid_argument &ex) {
        EXPECT_NE(std::string{ex.what()}.find("NOT NULL constraint violation on column name"),
                  std::string::npos);
    }
    const auto rows =
        executor.execute(parser.parse("SELECT name FROM People WHERE id = 1;"));
    ASSERT_TRUE(rows.success);
    ASSERT_EQ(rows.rows.size(), 1U);
    EXPECT_EQ(rows.rows[0][0], Value{std::string{"Ada"}});
}

TEST(ConstraintBehaviorTests, NotNullMultiRowInsertRefusesWithoutPartialWrite) {
    Parser parser;
    auto executor = makeTempExecutor("vertexdb-constraint-", "not-null-batch");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(
        executor.execute(parser.parse("CREATE TABLE People (id INT NOT NULL, name STRING);"))
            .success);
    EXPECT_THROW(
        (void)executor.execute(parser.parse(
            "INSERT INTO People VALUES (1, \"Ada\"), (2, NULL);")),
        std::invalid_argument);
    const auto rows = executor.execute(parser.parse("SELECT id FROM People;"));
    ASSERT_TRUE(rows.success);
    EXPECT_TRUE(rows.rows.empty());
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

TEST(ConstraintBehaviorTests, UniqueAutoIndexUsedForHashEq) {
    Parser parser;
    auto executor = makeTempExecutor("vertexdb-constraint-", "uq-hash-eq");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "CREATE TABLE Accounts (id INT PRIMARY KEY, email STRING UNIQUE);"))
                    .success);
    ASSERT_TRUE(
        executor.execute(parser.parse("INSERT INTO Accounts VALUES (1, \"a@x\");")).success);

    auto table = executor.currentDatabase()->table("Accounts");
    ASSERT_NE(table, nullptr);
    EXPECT_TRUE(table->hasIndex("email"));

    Select query{"Accounts",
                 {},
                 {SelectExpr::makeColumn("id")},
                 makeComparison("email", ComparisonOperator::Equal, Value{std::string{"a@x"}}),
                 {},
                 {}};
    const auto plan = QueryPlanner{}.planSelect(query, *table);
    EXPECT_EQ(plan.accessPath(), AccessPath::HashEq);
    EXPECT_EQ(std::get<HashEqPlan>(plan.path).indexColumn, "email");
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

TEST(ConstraintBehaviorTests, CannotDropUniqueConstraintIndex) {
    Parser parser;
    auto executor = makeTempExecutor("vertexdb-constraint-", "uq-drop-index");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "CREATE TABLE Accounts (id INT PRIMARY KEY, email STRING UNIQUE);"))
                    .success);
    const auto dropped = executor.execute(parser.parse("DROP INDEX __uq_email ON Accounts;"));
    EXPECT_FALSE(dropped.success);
    auto table = executor.currentDatabase()->table("Accounts");
    ASSERT_NE(table, nullptr);
    EXPECT_TRUE(table->hasIndex("email"));
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

TEST(ConstraintBehaviorTests, ParsesColumnAndTableCheckConstraints) {
    Parser parser;
    auto query = parser.parse(
        "CREATE TABLE Pay (id INT PRIMARY KEY, salary DOUBLE CHECK (salary > 0.0), "
        "bonus DOUBLE NULL, CHECK (salary > bonus));");
    ASSERT_TRUE(std::holds_alternative<CreateTable>(query));
    const auto &table = std::get<CreateTable>(query);
    ASSERT_EQ(table.columns.size(), 3U);
    ASSERT_EQ(table.checkConstraints.size(), 2U);
    EXPECT_EQ(predicateKind(table.checkConstraints[0]), PredicateKind::Comparison);
    EXPECT_EQ(predicateKind(table.checkConstraints[1]), PredicateKind::Comparison);
}

TEST(ConstraintBehaviorTests, RejectsUnsupportedCheckShapesAndUnknownColumns) {
    Parser parser;
    EXPECT_THROW((void)parser.parse("CREATE TABLE T (id INT CHECK (id IN (1, 2)));"),
                 std::runtime_error);
    EXPECT_THROW((void)parser.parse("CREATE TABLE T (id INT CHECK (name > 0));"),
                 std::runtime_error);
    EXPECT_THROW((void)parser.parse("CREATE TABLE T (id INT, CHECK (EXISTS (SELECT 1)));"),
                 std::runtime_error);
}

TEST(ConstraintBehaviorTests, CheckRejectsFailingInsert) {
    Parser parser;
    auto executor = makeTempExecutor("vertexdb-constraint-", "check-insert");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "CREATE TABLE Pay (id INT PRIMARY KEY, salary DOUBLE CHECK (salary > 0.0));"))
                    .success);
    try {
        (void)executor.execute(parser.parse("INSERT INTO Pay VALUES (1, 0.0);"));
        FAIL() << "expected CHECK rejection";
    } catch (const std::invalid_argument &ex) {
        EXPECT_NE(std::string{ex.what()}.find("CHECK constraint violation"), std::string::npos);
        EXPECT_NE(std::string{ex.what()}.find("salary > 0.0"), std::string::npos);
    }
    const auto rows = executor.execute(parser.parse("SELECT id FROM Pay;"));
    ASSERT_TRUE(rows.success);
    EXPECT_TRUE(rows.rows.empty());
}

TEST(ConstraintBehaviorTests, CheckRejectsFailingUpdate) {
    Parser parser;
    auto executor = makeTempExecutor("vertexdb-constraint-", "check-update");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "CREATE TABLE Pay (id INT PRIMARY KEY, salary DOUBLE CHECK (salary > 0.0));"))
                    .success);
    ASSERT_TRUE(executor.execute(parser.parse("INSERT INTO Pay VALUES (1, 10.0);")).success);
    try {
        (void)executor.execute(parser.parse("UPDATE Pay SET salary = -1.0 WHERE id = 1;"));
        FAIL() << "expected CHECK rejection";
    } catch (const std::invalid_argument &ex) {
        EXPECT_NE(std::string{ex.what()}.find("CHECK constraint violation"), std::string::npos);
    }
    const auto rows =
        executor.execute(parser.parse("SELECT salary FROM Pay WHERE id = 1;"));
    ASSERT_TRUE(rows.success);
    ASSERT_EQ(rows.rows.size(), 1U);
    EXPECT_EQ(rows.rows[0][0], Value{10.0});
}

TEST(ConstraintBehaviorTests, CheckAndOrAndNullUnknownPass) {
    Parser parser;
    auto executor = makeTempExecutor("vertexdb-constraint-", "check-and-null");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(
        executor
            .execute(parser.parse(
                "CREATE TABLE Pay (id INT PRIMARY KEY, salary DOUBLE NULL, "
                "bonus DOUBLE NULL, CHECK ((salary > 0.0) AND (bonus > 0.0)));"))
            .success);
    // NULL makes the predicate UNKNOWN → accepted.
    ASSERT_TRUE(
        executor.execute(parser.parse("INSERT INTO Pay VALUES (1, NULL, 5.0);")).success);
    ASSERT_TRUE(
        executor.execute(parser.parse("INSERT INTO Pay VALUES (2, 5.0, 1.0);")).success);
    EXPECT_THROW((void)executor.execute(parser.parse("INSERT INTO Pay VALUES (3, -1.0, 1.0);")),
                 std::invalid_argument);
    const auto rows = executor.execute(parser.parse("SELECT id FROM Pay ORDER BY id;"));
    ASSERT_TRUE(rows.success);
    ASSERT_EQ(rows.rows.size(), 2U);
}

TEST(ConstraintBehaviorTests, CheckMultiRowInsertRefusesWithoutPartialWrite) {
    Parser parser;
    auto executor = makeTempExecutor("vertexdb-constraint-", "check-batch");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "CREATE TABLE Pay (id INT PRIMARY KEY, salary DOUBLE CHECK (salary > 0.0));"))
                    .success);
    EXPECT_THROW(
        (void)executor.execute(parser.parse("INSERT INTO Pay VALUES (1, 10.0), (2, -1.0);")),
        std::invalid_argument);
    const auto rows = executor.execute(parser.parse("SELECT id FROM Pay;"));
    ASSERT_TRUE(rows.success);
    EXPECT_TRUE(rows.rows.empty());
}

TEST(ConstraintBehaviorTests, SaveLoadPreservesCheckConstraints) {
    Parser parser;
    auto executor = makeTempExecutor("vertexdb-constraint-", "check-save-load");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "CREATE TABLE Pay (id INT PRIMARY KEY, salary DOUBLE, bonus DOUBLE, "
                        "CHECK (salary > bonus));"))
                    .success);
    ASSERT_TRUE(
        executor.execute(parser.parse("INSERT INTO Pay VALUES (1, 10.0, 1.0);")).success);
    ASSERT_TRUE(executor.execute(parser.parse("SAVE DATABASE;")).success);
    ASSERT_TRUE(executor.execute(parser.parse("LOAD DATABASE company;")).success);

    auto table = executor.currentDatabase()->table("Pay");
    ASSERT_NE(table, nullptr);
    ASSERT_EQ(table->checkConstraints().size(), 1U);

    EXPECT_THROW((void)executor.execute(parser.parse("INSERT INTO Pay VALUES (2, 1.0, 10.0);")),
                 std::invalid_argument);
    ASSERT_TRUE(
        executor.execute(parser.parse("INSERT INTO Pay VALUES (2, 20.0, 5.0);")).success);
}

TEST(ConstraintBehaviorTests, ParsesColumnAndTableForeignKeys) {
    Parser parser;
    auto query = parser.parse(
        "CREATE TABLE Orders (id INT PRIMARY KEY, customer_id INT REFERENCES Customers(id), "
        "FOREIGN KEY (customer_id) REFERENCES Customers(id) ON DELETE NO ACTION);");
    ASSERT_TRUE(std::holds_alternative<CreateTable>(query));
    const auto &table = std::get<CreateTable>(query);
    ASSERT_EQ(table.foreignKeys.size(), 2U);
    EXPECT_EQ(table.foreignKeys[0].childColumns.size(), 1U);
    EXPECT_EQ(table.foreignKeys[0].childColumns[0], "customer_id");
    EXPECT_EQ(table.foreignKeys[0].parentTable, "Customers");
    EXPECT_EQ(table.foreignKeys[0].parentColumns.size(), 1U);
    EXPECT_EQ(table.foreignKeys[0].parentColumns[0], "id");
    EXPECT_EQ(table.foreignKeys[1].childColumns[0], "customer_id");
}

TEST(ConstraintBehaviorTests, CreateForeignKeyRejectsMissingOrNonUniqueParent) {
    Parser parser;
    auto executor = makeTempExecutor("vertexdb-constraint-", "fk-create-reject");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    auto missing = executor.execute(parser.parse(
        "CREATE TABLE Orders (id INT PRIMARY KEY, customer_id INT REFERENCES Customers(id));"));
    EXPECT_FALSE(missing.success);
    EXPECT_NE(missing.message.find("parent table not found"), std::string::npos);

    ASSERT_TRUE(
        executor.execute(parser.parse("CREATE TABLE Customers (id INT, name STRING);")).success);
    auto nonUnique = executor.execute(parser.parse(
        "CREATE TABLE Orders (id INT PRIMARY KEY, customer_id INT REFERENCES Customers(id));"));
    EXPECT_FALSE(nonUnique.success);
    EXPECT_NE(nonUnique.message.find("PRIMARY KEY or UNIQUE"), std::string::npos);
}

TEST(ConstraintBehaviorTests, ForeignKeyRejectsOrphanInsertAndAllowsNull) {
    Parser parser;
    auto executor = makeTempExecutor("vertexdb-constraint-", "fk-insert");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(
        executor.execute(parser.parse("CREATE TABLE Customers (id INT PRIMARY KEY);")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "CREATE TABLE Orders (id INT PRIMARY KEY, customer_id INT NULL "
                        "REFERENCES Customers(id));"))
                    .success);
    ASSERT_TRUE(executor.execute(parser.parse("INSERT INTO Customers VALUES (1);")).success);
    ASSERT_TRUE(
        executor.execute(parser.parse("INSERT INTO Orders VALUES (10, 1);")).success);
    ASSERT_TRUE(
        executor.execute(parser.parse("INSERT INTO Orders VALUES (11, NULL);")).success);
    try {
        (void)executor.execute(parser.parse("INSERT INTO Orders VALUES (12, 99);"));
        FAIL() << "expected FOREIGN KEY rejection";
    } catch (const std::invalid_argument &ex) {
        EXPECT_NE(std::string{ex.what()}.find("FOREIGN KEY constraint violation on column customer_id"),
                  std::string::npos);
    }
}

TEST(ConstraintBehaviorTests, ForeignKeyRejectsOrphanUpdate) {
    Parser parser;
    auto executor = makeTempExecutor("vertexdb-constraint-", "fk-update");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(
        executor.execute(parser.parse("CREATE TABLE Customers (id INT PRIMARY KEY);")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "CREATE TABLE Orders (id INT PRIMARY KEY, customer_id INT "
                        "REFERENCES Customers(id));"))
                    .success);
    ASSERT_TRUE(executor.execute(parser.parse("INSERT INTO Customers VALUES (1), (2);")).success);
    ASSERT_TRUE(executor.execute(parser.parse("INSERT INTO Orders VALUES (10, 1);")).success);
    EXPECT_THROW(
        (void)executor.execute(parser.parse("UPDATE Orders SET customer_id = 99 WHERE id = 10;")),
        std::invalid_argument);
    ASSERT_TRUE(
        executor.execute(parser.parse("UPDATE Orders SET customer_id = 2 WHERE id = 10;")).success);
}

TEST(ConstraintBehaviorTests, ForeignKeyRejectsParentDeleteAndKeyUpdate) {
    Parser parser;
    auto executor = makeTempExecutor("vertexdb-constraint-", "fk-parent-delete");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(
        executor.execute(parser.parse("CREATE TABLE Customers (id INT PRIMARY KEY);")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "CREATE TABLE Orders (id INT PRIMARY KEY, customer_id INT "
                        "REFERENCES Customers(id));"))
                    .success);
    ASSERT_TRUE(executor.execute(parser.parse("INSERT INTO Customers VALUES (1);")).success);
    ASSERT_TRUE(executor.execute(parser.parse("INSERT INTO Orders VALUES (10, 1);")).success);
    EXPECT_THROW((void)executor.execute(parser.parse("DELETE FROM Customers WHERE id = 1;")),
                 std::invalid_argument);
    EXPECT_THROW((void)executor.execute(parser.parse("UPDATE Customers SET id = 2 WHERE id = 1;")),
                 std::invalid_argument);
    ASSERT_TRUE(executor.execute(parser.parse("DELETE FROM Orders WHERE id = 10;")).success);
    ASSERT_TRUE(executor.execute(parser.parse("DELETE FROM Customers WHERE id = 1;")).success);
}

TEST(ConstraintBehaviorTests, ForeignKeyMultiRowInsertRefusesWithoutPartialWrite) {
    Parser parser;
    auto executor = makeTempExecutor("vertexdb-constraint-", "fk-batch");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(
        executor.execute(parser.parse("CREATE TABLE Customers (id INT PRIMARY KEY);")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "CREATE TABLE Orders (id INT PRIMARY KEY, customer_id INT "
                        "REFERENCES Customers(id));"))
                    .success);
    ASSERT_TRUE(executor.execute(parser.parse("INSERT INTO Customers VALUES (1);")).success);
    EXPECT_THROW(
        (void)executor.execute(parser.parse("INSERT INTO Orders VALUES (10, 1), (11, 99);")),
        std::invalid_argument);
    const auto rows = executor.execute(parser.parse("SELECT id FROM Orders;"));
    ASSERT_TRUE(rows.success);
    EXPECT_TRUE(rows.rows.empty());
}

TEST(ConstraintBehaviorTests, DropParentTableRejectedWhileReferenced) {
    Parser parser;
    auto executor = makeTempExecutor("vertexdb-constraint-", "fk-drop-parent");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(
        executor.execute(parser.parse("CREATE TABLE Customers (id INT PRIMARY KEY);")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "CREATE TABLE Orders (id INT PRIMARY KEY, customer_id INT "
                        "REFERENCES Customers(id));"))
                    .success);
    auto dropped = executor.execute(parser.parse("DROP TABLE Customers;"));
    EXPECT_FALSE(dropped.success);
    EXPECT_NE(dropped.message.find("FOREIGN KEY"), std::string::npos);
}

TEST(ConstraintBehaviorTests, SaveLoadPreservesForeignKeys) {
    Parser parser;
    auto executor = makeTempExecutor("vertexdb-constraint-", "fk-save-load");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(
        executor.execute(parser.parse("CREATE TABLE Customers (id INT PRIMARY KEY);")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "CREATE TABLE Orders (id INT PRIMARY KEY, customer_id INT "
                        "REFERENCES Customers(id));"))
                    .success);
    ASSERT_TRUE(executor.execute(parser.parse("INSERT INTO Customers VALUES (1);")).success);
    ASSERT_TRUE(executor.execute(parser.parse("INSERT INTO Orders VALUES (10, 1);")).success);
    ASSERT_TRUE(executor.execute(parser.parse("SAVE DATABASE;")).success);
    ASSERT_TRUE(executor.execute(parser.parse("LOAD DATABASE company;")).success);

    auto table = executor.currentDatabase()->table("Orders");
    ASSERT_NE(table, nullptr);
    ASSERT_EQ(table->foreignKeys().size(), 1U);
    EXPECT_EQ(table->foreignKeys()[0].parentTable, "Customers");

    EXPECT_THROW((void)executor.execute(parser.parse("INSERT INTO Orders VALUES (11, 99);")),
                 std::invalid_argument);
    EXPECT_THROW((void)executor.execute(parser.parse("DELETE FROM Customers WHERE id = 1;")),
                 std::invalid_argument);
}

TEST(ConstraintBehaviorTests, ParsesOnUpdateNoActionAndEmitsForeignKeySql) {
    Parser parser;
    auto query = parser.parse(
        "CREATE TABLE Orders (id INT PRIMARY KEY, customer_id INT, "
        "FOREIGN KEY (customer_id) REFERENCES Customers(id) ON UPDATE NO ACTION ON DELETE NO ACTION);");
    ASSERT_TRUE(std::holds_alternative<CreateTable>(query));
    const auto &table = std::get<CreateTable>(query);
    ASSERT_EQ(table.foreignKeys.size(), 1U);
    const auto sql = createTableSql(table);
    EXPECT_NE(sql.find("FOREIGN KEY (customer_id) REFERENCES Customers(id)"), std::string::npos);
}

TEST(ConstraintBehaviorTests, CreateForeignKeyRejectsTypeMismatchAndUnknownParentColumn) {
    Parser parser;
    auto executor = makeTempExecutor("vertexdb-constraint-", "fk-create-type");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(
        executor.execute(parser.parse("CREATE TABLE Customers (id INT PRIMARY KEY);")).success);

    auto badType = executor.execute(parser.parse(
        "CREATE TABLE Orders (id INT PRIMARY KEY, customer_id STRING REFERENCES Customers(id));"));
    EXPECT_FALSE(badType.success);
    EXPECT_NE(badType.message.find("type mismatch"), std::string::npos);

    auto badParentCol = executor.execute(parser.parse(
        "CREATE TABLE Orders (id INT PRIMARY KEY, customer_id INT REFERENCES Customers(missing));"));
    EXPECT_FALSE(badParentCol.success);
    EXPECT_NE(badParentCol.message.find("parent column not found"), std::string::npos);
}

TEST(ConstraintBehaviorTests, ValidateForeignKeyDefinitionsRejectsUnsupportedActions) {
    Database database{"company"};
    ASSERT_TRUE(database.createTable("Customers", {{"id", ColumnType::Int, false, true, true}}));
    std::vector<Column> childSchema{{"id", ColumnType::Int, false, true, true},
                                    {"customer_id", ColumnType::Int, true, false, false}};
    ForeignKeyConstraint fk{"customer_id", "Customers", "id"};
    // Force an unsupported action value past the parser.
    fk.onDelete = static_cast<ForeignKeyAction>(255);
    std::vector<ForeignKeyConstraint> badAction{fk};
    EXPECT_THROW(validateForeignKeyDefinitions(database, "Orders", childSchema, badAction),
                 std::invalid_argument);

    ForeignKeyConstraint setNullNotNull{"customer_id", "Customers", "id"};
    setNullNotNull.onDelete = ForeignKeyAction::SetNull;
    std::vector<Column> notNullChild{{"id", ColumnType::Int, false, true, true},
                                     {"customer_id", ColumnType::Int, false, false, false}};
    std::vector<ForeignKeyConstraint> badSetNull{setNullNotNull};
    EXPECT_THROW(validateForeignKeyDefinitions(database, "Orders", notNullChild, badSetNull),
                 std::invalid_argument);

    ForeignKeyConstraint missingChild{"nope", "Customers", "id"};
    std::vector<ForeignKeyConstraint> badChild{missingChild};
    EXPECT_THROW(validateForeignKeyDefinitions(database, "Orders", childSchema, badChild),
                 std::invalid_argument);
}

TEST(ConstraintBehaviorTests, SelfReferentialForeignKeyAllowsSameTxnParentThenChild) {
    Parser parser;
    auto executor = makeTempExecutor("vertexdb-constraint-", "fk-self");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "CREATE TABLE Employees (id INT PRIMARY KEY, manager_id INT NULL "
                        "REFERENCES Employees(id));"))
                    .success);
    ASSERT_TRUE(executor.execute(parser.parse("INSERT INTO Employees VALUES (1, NULL);")).success);
    ASSERT_TRUE(executor.execute(parser.parse("INSERT INTO Employees VALUES (2, 1);")).success);
    EXPECT_THROW((void)executor.execute(parser.parse("INSERT INTO Employees VALUES (3, 99);")),
                 std::invalid_argument);
    EXPECT_THROW((void)executor.execute(parser.parse("DELETE FROM Employees WHERE id = 1;")),
                 std::invalid_argument);
    ASSERT_TRUE(executor.execute(parser.parse("DELETE FROM Employees WHERE id = 2;")).success);
    ASSERT_TRUE(executor.execute(parser.parse("DELETE FROM Employees WHERE id = 1;")).success);
}

TEST(ConstraintBehaviorTests, RenameParentTableRejectedWhileReferenced) {
    Parser parser;
    auto executor = makeTempExecutor("vertexdb-constraint-", "fk-rename-parent");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(
        executor.execute(parser.parse("CREATE TABLE Customers (id INT PRIMARY KEY);")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "CREATE TABLE Orders (id INT PRIMARY KEY, customer_id INT "
                        "REFERENCES Customers(id));"))
                    .success);
    auto renamed = executor.execute(parser.parse("RENAME TABLE Customers TO Clients;"));
    EXPECT_FALSE(renamed.success);
    EXPECT_NE(renamed.message.find("FOREIGN KEY"), std::string::npos);
}

TEST(ConstraintBehaviorTests, ParentNonKeyUpdateAndSameKeyUpdateAllowedWhileReferenced) {
    Parser parser;
    auto executor = makeTempExecutor("vertexdb-constraint-", "fk-parent-nontarget");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "CREATE TABLE Customers (id INT PRIMARY KEY, name STRING);"))
                    .success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "CREATE TABLE Orders (id INT PRIMARY KEY, customer_id INT "
                        "REFERENCES Customers(id));"))
                    .success);
    ASSERT_TRUE(
        executor.execute(parser.parse("INSERT INTO Customers VALUES (1, \"Ada\");")).success);
    ASSERT_TRUE(executor.execute(parser.parse("INSERT INTO Orders VALUES (10, 1);")).success);
    ASSERT_TRUE(
        executor.execute(parser.parse("UPDATE Customers SET name = \"Ada Lovelace\" WHERE id = 1;"))
            .success);
    ASSERT_TRUE(
        executor.execute(parser.parse("UPDATE Customers SET id = 1 WHERE id = 1;")).success);
}

TEST(ConstraintBehaviorTests, TableForeignKeyWithoutChildIndexUsesScanOnParentDelete) {
    Parser parser;
    auto executor = makeTempExecutor("vertexdb-constraint-", "fk-scan-delete");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(
        executor.execute(parser.parse("CREATE TABLE Customers (id INT PRIMARY KEY);")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "CREATE TABLE Orders (id INT PRIMARY KEY, customer_id INT, "
                        "FOREIGN KEY (customer_id) REFERENCES Customers(id));"))
                    .success);
    ASSERT_TRUE(executor.execute(parser.parse("INSERT INTO Customers VALUES (1), (2);")).success);
    ASSERT_TRUE(executor.execute(parser.parse("INSERT INTO Orders VALUES (10, 1);")).success);
    EXPECT_THROW((void)executor.execute(parser.parse("DELETE FROM Customers WHERE id = 1;")),
                 std::invalid_argument);
    ASSERT_TRUE(executor.execute(parser.parse("DELETE FROM Customers WHERE id = 2;")).success);
}

TEST(ConstraintBehaviorTests, CheckOrEqualLessAndStringLiteralPaths) {
    // Desired: CHECK supports OR / = / < and string literals (incl. escapes) in messages/SQL.
    Parser parser;
    auto executor = makeTempExecutor("vertexdb-constraint-", "check-or-eq-lt");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "CREATE TABLE Flags (id INT PRIMARY KEY, a INT NULL, b INT NULL, "
                        "label STRING, CHECK ((a = 1) OR (b < 0)), CHECK (label = \"ok\\\"x\"));"))
                    .success);

    ASSERT_TRUE(
        executor.execute(parser.parse("INSERT INTO Flags VALUES (1, 1, 5, \"ok\\\"x\");"))
            .success);
    ASSERT_TRUE(
        executor.execute(parser.parse("INSERT INTO Flags VALUES (2, 0, -1, \"ok\\\"x\");"))
            .success);
    // OR both false → reject.
    EXPECT_THROW(
        (void)executor.execute(parser.parse("INSERT INTO Flags VALUES (3, 0, 5, \"ok\\\"x\");")),
        std::invalid_argument);
    // String CHECK reject.
    EXPECT_THROW(
        (void)executor.execute(parser.parse("INSERT INTO Flags VALUES (4, 1, 5, \"no\");")),
        std::invalid_argument);
    // NULL arm of OR is UNKNOWN; other false → still UNKNOWN → accept.
    ASSERT_TRUE(
        executor.execute(parser.parse("INSERT INTO Flags VALUES (5, NULL, 5, \"ok\\\"x\");"))
            .success);

    auto table = executor.currentDatabase()->table("Flags");
    ASSERT_NE(table, nullptr);
    ASSERT_EQ(table->checkConstraints().size(), 2U);
    const auto orLiteral = checkConstraintLiteral(table->checkConstraints()[0]);
    EXPECT_NE(orLiteral.find(" OR "), std::string::npos);
    EXPECT_NE(orLiteral.find("a = 1"), std::string::npos);
    EXPECT_NE(orLiteral.find("b < 0"), std::string::npos);
    const auto stringLiteral = checkConstraintLiteral(table->checkConstraints()[1]);
    EXPECT_NE(stringLiteral.find("label = \"ok\\\"x\""), std::string::npos);
}

TEST(ConstraintBehaviorTests, CatalogCommandsRequireActiveDatabase) {
    Parser parser;
    auto executor = makeTempExecutor("vertexdb-constraint-", "catalog-no-db");

    auto createTable = executor.execute(parser.parse("CREATE TABLE T (id INT);"));
    EXPECT_FALSE(createTable.success);
    EXPECT_NE(createTable.message.find("no active database"), std::string::npos);

    auto dropTable = executor.execute(parser.parse("DROP TABLE T;"));
    EXPECT_FALSE(dropTable.success);
    EXPECT_NE(dropTable.message.find("no active database"), std::string::npos);

    auto rename = executor.execute(parser.parse("RENAME TABLE T TO U;"));
    EXPECT_FALSE(rename.success);
    EXPECT_NE(rename.message.find("no active database"), std::string::npos);

    auto list = executor.execute(parser.parse("LIST TABLES;"));
    EXPECT_FALSE(list.success);
    EXPECT_NE(list.message.find("no active database"), std::string::npos);

    auto save = executor.execute(parser.parse("SAVE DATABASE;"));
    EXPECT_FALSE(save.success);
    EXPECT_NE(save.message.find("no active database"), std::string::npos);

    auto dropDb = executor.execute(parser.parse("DROP DATABASE missing;"));
    EXPECT_FALSE(dropDb.success);
    EXPECT_NE(dropDb.message.find("no active database"), std::string::npos);

    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    auto wrongDrop = executor.execute(parser.parse("DROP DATABASE other;"));
    EXPECT_FALSE(wrongDrop.success);
    EXPECT_NE(wrongDrop.message.find("active database"), std::string::npos);

    EXPECT_THROW((void)executor.execute(parser.parse("UPDATE missing SET id = 1;")),
                 std::runtime_error);
    ASSERT_TRUE(executor.execute(parser.parse("CREATE TABLE T (id INT);")).success);
    EXPECT_THROW((void)executor.execute(parser.parse("UPDATE T SET nosuch = 1;")),
                 std::runtime_error);
}

TEST(ConstraintBehaviorTests, ParsesCompositePrimaryKeyAndUnique) {
    Parser parser;
    auto query = parser.parse(
        "CREATE TABLE Enrollments (student_id INT, course_id INT, note STRING NULL, "
        "PRIMARY KEY (student_id, course_id), UNIQUE (note, course_id));");
    ASSERT_TRUE(std::holds_alternative<CreateTable>(query));
    const auto &table = std::get<CreateTable>(query);
    ASSERT_EQ(table.uniqueConstraints.size(), 2U);
    EXPECT_TRUE(table.uniqueConstraints[0].primaryKey);
    ASSERT_EQ(table.uniqueConstraints[0].columns.size(), 2U);
    EXPECT_EQ(table.uniqueConstraints[0].columns[0], "student_id");
    EXPECT_EQ(table.uniqueConstraints[0].columns[1], "course_id");
    EXPECT_FALSE(table.columns[0].nullable);
    EXPECT_FALSE(table.columns[1].nullable);
    EXPECT_FALSE(table.uniqueConstraints[1].primaryKey);
}

TEST(ConstraintBehaviorTests, CompositePrimaryKeyRejectsDuplicatesAndAllowsIndexedLookup) {
    Parser parser;
    auto executor = makeTempExecutor("vertexdb-constraint-", "composite-pk");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE school;")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "CREATE TABLE Enrollments (student_id INT, course_id INT, grade STRING, "
                        "PRIMARY KEY (student_id, course_id));"))
                    .success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "INSERT INTO Enrollments VALUES (1, 10, \"A\"), (1, 11, \"B\");"))
                    .success);
    EXPECT_THROW((void)executor.execute(
                     parser.parse("INSERT INTO Enrollments VALUES (1, 10, \"C\");")),
                 std::invalid_argument);

    auto table = executor.currentDatabase()->table("Enrollments");
    ASSERT_TRUE(table);
    const std::vector<std::string> keyCols{"student_id", "course_id"};
    EXPECT_TRUE(table->hasIndex(keyCols));
    const auto names = table->listIndexes();
    EXPECT_NE(std::find(names.begin(), names.end(), "__pk_student_id_course_id"), names.end());

    const auto key = Value::composite({Value{1}, Value{10}});
    auto hits = table->indexedLookup(keyCols, key);
    ASSERT_TRUE(hits);
    ASSERT_EQ(hits->size(), 1U);
}

TEST(ConstraintBehaviorTests, CompositeUniqueAllowsNullPartsAndRejectsFullKeyDup) {
    Parser parser;
    auto executor = makeTempExecutor("vertexdb-constraint-", "composite-uq");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "CREATE TABLE Tags (id INT PRIMARY KEY, a INT NULL, b INT NULL, "
                        "UNIQUE (a, b));"))
                    .success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "INSERT INTO Tags VALUES (1, 1, NULL), (2, 1, NULL), (3, 1, 2);"))
                    .success);
    EXPECT_THROW((void)executor.execute(parser.parse("INSERT INTO Tags VALUES (4, 1, 2);")),
                 std::invalid_argument);
    const auto rows = executor.execute(parser.parse("SELECT id FROM Tags ORDER BY id;"));
    ASSERT_TRUE(rows.success);
    ASSERT_EQ(rows.rows.size(), 3U);
}

TEST(ConstraintBehaviorTests, CompositeConstraintsSurviveSaveLoad) {
    Parser parser;
    auto executor = makeTempExecutor("vertexdb-constraint-", "composite-save");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "CREATE TABLE Pair (a INT, b INT, PRIMARY KEY (a, b));"))
                    .success);
    ASSERT_TRUE(executor.execute(parser.parse("INSERT INTO Pair VALUES (1, 2);")).success);
    ASSERT_TRUE(executor.execute(parser.parse("SAVE DATABASE;")).success);
    ASSERT_TRUE(executor.execute(parser.parse("LOAD DATABASE;")).success);
    EXPECT_THROW((void)executor.execute(parser.parse("INSERT INTO Pair VALUES (1, 2);")),
                 std::invalid_argument);
    auto table = executor.currentDatabase()->table("Pair");
    ASSERT_TRUE(table);
    ASSERT_EQ(table->uniqueConstraints().size(), 1U);
    EXPECT_TRUE(table->uniqueConstraints()[0].primaryKey);
}

TEST(ConstraintBehaviorTests, CreateIndexSupportsCompositeColumns) {
    Parser parser;
    auto query = parser.parse("CREATE INDEX idx_ab ON T (a, b);");
    ASSERT_TRUE(std::holds_alternative<CreateIndex>(query));
    const auto &index = std::get<CreateIndex>(query);
    ASSERT_EQ(index.columns.size(), 2U);
    EXPECT_EQ(index.columns[0], "a");
    EXPECT_EQ(index.columns[1], "b");
    EXPECT_EQ(index.column, "a");
}

TEST(ConstraintBehaviorTests, ParsesCascadeAndSetNullActions) {
    Parser parser;
    auto query = parser.parse(
        "CREATE TABLE Orders (id INT PRIMARY KEY, customer_id INT NULL, "
        "FOREIGN KEY (customer_id) REFERENCES Customers(id) "
        "ON DELETE CASCADE ON UPDATE SET NULL);");
    ASSERT_TRUE(std::holds_alternative<CreateTable>(query));
    const auto &table = std::get<CreateTable>(query);
    ASSERT_EQ(table.foreignKeys.size(), 1U);
    EXPECT_EQ(table.foreignKeys[0].onDelete, ForeignKeyAction::Cascade);
    EXPECT_EQ(table.foreignKeys[0].onUpdate, ForeignKeyAction::SetNull);
    EXPECT_NE(createTableSql(table).find("ON DELETE CASCADE"), std::string::npos);
    EXPECT_NE(createTableSql(table).find("ON UPDATE SET NULL"), std::string::npos);
}

TEST(ConstraintBehaviorTests, ForeignKeyOnDeleteCascadeRemovesChildren) {
    Parser parser;
    auto executor = makeTempExecutor("vertexdb-constraint-", "fk-cascade-del");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(
        executor.execute(parser.parse("CREATE TABLE Customers (id INT PRIMARY KEY);")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "CREATE TABLE Orders (id INT PRIMARY KEY, customer_id INT, "
                        "FOREIGN KEY (customer_id) REFERENCES Customers(id) ON DELETE CASCADE);"))
                    .success);
    ASSERT_TRUE(executor.execute(parser.parse("INSERT INTO Customers VALUES (1), (2);")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "INSERT INTO Orders VALUES (10, 1), (11, 1), (20, 2);"))
                    .success);
    ASSERT_TRUE(executor.execute(parser.parse("DELETE FROM Customers WHERE id = 1;")).success);
    const auto rows = executor.execute(parser.parse("SELECT id FROM Orders ORDER BY id;"));
    ASSERT_TRUE(rows.success);
    ASSERT_EQ(rows.rows.size(), 1U);
    EXPECT_EQ(rows.rows[0][0], Value{20});
}

TEST(ConstraintBehaviorTests, ForeignKeyCascadeDepthExceeded) {
    // Documented limitation: ON DELETE CASCADE chains deeper than kMaxReferentialActionDepth
    // are rejected (self-FK chain of depth+1 descendants).
    Parser parser;
    auto executor = makeTempExecutor("vertexdb-constraint-", "fk-cascade-depth");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "CREATE TABLE Nodes (id INT PRIMARY KEY, parent_id INT NULL, "
                        "FOREIGN KEY (parent_id) REFERENCES Nodes(id) ON DELETE CASCADE);"))
                    .success);
    ASSERT_TRUE(executor.execute(parser.parse("INSERT INTO Nodes VALUES (0, NULL);")).success);
    for (std::size_t i = 1; i <= kMaxReferentialActionDepth; ++i) {
        ASSERT_TRUE(executor
                        .execute(parser.parse("INSERT INTO Nodes VALUES (" + std::to_string(i) +
                                              ", " + std::to_string(i - 1) + ");"))
                        .success);
    }
    try {
        (void)executor.execute(parser.parse("DELETE FROM Nodes WHERE id = 0;"));
        FAIL() << "expected FOREIGN KEY CASCADE depth exceeded";
    } catch (const std::invalid_argument &ex) {
        EXPECT_NE(std::string{ex.what()}.find("CASCADE depth exceeded"), std::string::npos);
    }
}

TEST(ConstraintBehaviorTests, ForeignKeyOnDeleteSetNullNullsChildren) {
    Parser parser;
    auto executor = makeTempExecutor("vertexdb-constraint-", "fk-setnull-del");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(
        executor.execute(parser.parse("CREATE TABLE Customers (id INT PRIMARY KEY);")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "CREATE TABLE Orders (id INT PRIMARY KEY, customer_id INT NULL, "
                        "FOREIGN KEY (customer_id) REFERENCES Customers(id) ON DELETE SET NULL);"))
                    .success);
    ASSERT_TRUE(executor.execute(parser.parse("INSERT INTO Customers VALUES (1);")).success);
    ASSERT_TRUE(executor.execute(parser.parse("INSERT INTO Orders VALUES (10, 1);")).success);
    ASSERT_TRUE(executor.execute(parser.parse("DELETE FROM Customers WHERE id = 1;")).success);
    const auto rows = executor.execute(parser.parse("SELECT customer_id FROM Orders;"));
    ASSERT_TRUE(rows.success);
    ASSERT_EQ(rows.rows.size(), 1U);
    EXPECT_TRUE(rows.rows[0][0].isNull());
}

TEST(ConstraintBehaviorTests, ForeignKeyOnUpdateCascadePropagatesNewKey) {
    Parser parser;
    auto executor = makeTempExecutor("vertexdb-constraint-", "fk-cascade-upd");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(
        executor.execute(parser.parse("CREATE TABLE Customers (id INT PRIMARY KEY);")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "CREATE TABLE Orders (id INT PRIMARY KEY, customer_id INT, "
                        "FOREIGN KEY (customer_id) REFERENCES Customers(id) ON UPDATE CASCADE);"))
                    .success);
    ASSERT_TRUE(executor.execute(parser.parse("INSERT INTO Customers VALUES (1);")).success);
    ASSERT_TRUE(executor.execute(parser.parse("INSERT INTO Orders VALUES (10, 1);")).success);
    ASSERT_TRUE(executor.execute(parser.parse("UPDATE Customers SET id = 7 WHERE id = 1;")).success);
    const auto rows = executor.execute(parser.parse("SELECT customer_id FROM Orders;"));
    ASSERT_TRUE(rows.success);
    ASSERT_EQ(rows.rows.size(), 1U);
    EXPECT_EQ(rows.rows[0][0], Value{7});
}

TEST(ConstraintBehaviorTests, ForeignKeyOnUpdateSetNullClearsChildren) {
    Parser parser;
    auto executor = makeTempExecutor("vertexdb-constraint-", "fk-setnull-upd");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(
        executor.execute(parser.parse("CREATE TABLE Customers (id INT PRIMARY KEY);")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "CREATE TABLE Orders (id INT PRIMARY KEY, customer_id INT NULL, "
                        "FOREIGN KEY (customer_id) REFERENCES Customers(id) ON UPDATE SET NULL);"))
                    .success);
    ASSERT_TRUE(executor.execute(parser.parse("INSERT INTO Customers VALUES (1);")).success);
    ASSERT_TRUE(executor.execute(parser.parse("INSERT INTO Orders VALUES (10, 1);")).success);
    ASSERT_TRUE(executor.execute(parser.parse("UPDATE Customers SET id = 7 WHERE id = 1;")).success);
    const auto rows = executor.execute(parser.parse("SELECT customer_id FROM Orders;"));
    ASSERT_TRUE(rows.success);
    ASSERT_EQ(rows.rows.size(), 1U);
    EXPECT_TRUE(rows.rows[0][0].isNull());
}

TEST(ConstraintBehaviorTests, ForeignKeySetNullRejectedOnNotNullChild) {
    Parser parser;
    auto executor = makeTempExecutor("vertexdb-constraint-", "fk-setnull-nn");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(
        executor.execute(parser.parse("CREATE TABLE Customers (id INT PRIMARY KEY);")).success);
    auto created = executor.execute(parser.parse(
        "CREATE TABLE Orders (id INT PRIMARY KEY, customer_id INT, "
        "FOREIGN KEY (customer_id) REFERENCES Customers(id) ON DELETE SET NULL);"));
    EXPECT_FALSE(created.success);
    EXPECT_NE(created.message.find("nullable"), std::string::npos);
}

TEST(ConstraintBehaviorTests, ForeignKeyCascadeDeleteSurvivesSaveLoad) {
    Parser parser;
    auto executor = makeTempExecutor("vertexdb-constraint-", "fk-cascade-save");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(
        executor.execute(parser.parse("CREATE TABLE Customers (id INT PRIMARY KEY);")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "CREATE TABLE Orders (id INT PRIMARY KEY, customer_id INT, "
                        "FOREIGN KEY (customer_id) REFERENCES Customers(id) ON DELETE CASCADE);"))
                    .success);
    ASSERT_TRUE(executor.execute(parser.parse("INSERT INTO Customers VALUES (1);")).success);
    ASSERT_TRUE(executor.execute(parser.parse("INSERT INTO Orders VALUES (10, 1);")).success);
    ASSERT_TRUE(executor.execute(parser.parse("SAVE DATABASE;")).success);
    ASSERT_TRUE(executor.execute(parser.parse("LOAD DATABASE;")).success);
    ASSERT_TRUE(executor.execute(parser.parse("DELETE FROM Customers WHERE id = 1;")).success);
    const auto rows = executor.execute(parser.parse("SELECT id FROM Orders;"));
    ASSERT_TRUE(rows.success);
    EXPECT_TRUE(rows.rows.empty());
}

TEST(ConstraintBehaviorTests, ParsesCompositeForeignKey) {
    Parser parser;
    const auto query = parser.parse(
        "CREATE TABLE LineItems (order_id INT NULL, item_id INT NULL, qty INT, "
        "FOREIGN KEY (order_id, item_id) REFERENCES Catalog(order_id, item_id) "
        "ON DELETE CASCADE ON UPDATE SET NULL);");
    ASSERT_TRUE(std::holds_alternative<CreateTable>(query));
    const auto &table = std::get<CreateTable>(query);
    ASSERT_EQ(table.foreignKeys.size(), 1U);
    ASSERT_EQ(table.foreignKeys[0].childColumns.size(), 2U);
    EXPECT_EQ(table.foreignKeys[0].childColumns[0], "order_id");
    EXPECT_EQ(table.foreignKeys[0].childColumns[1], "item_id");
    ASSERT_EQ(table.foreignKeys[0].parentColumns.size(), 2U);
    EXPECT_EQ(table.foreignKeys[0].parentColumns[0], "order_id");
    EXPECT_EQ(table.foreignKeys[0].parentColumns[1], "item_id");
    EXPECT_EQ(table.foreignKeys[0].onDelete, ForeignKeyAction::Cascade);
    EXPECT_EQ(table.foreignKeys[0].onUpdate, ForeignKeyAction::SetNull);
    const auto sql = foreignKeyLiteral(table.foreignKeys[0]);
    EXPECT_NE(sql.find("FOREIGN KEY (order_id, item_id) REFERENCES Catalog(order_id, item_id)"),
              std::string::npos);
}

TEST(ConstraintBehaviorTests, CreateCompositeForeignKeyRejectsCountMismatchAndNonExactUnique) {
    Parser parser;
    auto executor = makeTempExecutor("vertexdb-constraint-", "fk-composite-reject");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "CREATE TABLE Catalog (a INT, b INT, c INT, PRIMARY KEY (a, b, c));"))
                    .success);
    EXPECT_THROW(
        (void)parser.parse(
            "CREATE TABLE Child (a INT, b INT, FOREIGN KEY (a, b) REFERENCES Catalog(a));"),
        std::runtime_error);

    auto subset = executor.execute(parser.parse(
        "CREATE TABLE Child2 (a INT, b INT, FOREIGN KEY (a, b) REFERENCES Catalog(a, b));"));
    EXPECT_FALSE(subset.success);
    EXPECT_NE(subset.message.find("PRIMARY KEY or UNIQUE"), std::string::npos);
}

TEST(ConstraintBehaviorTests, CompositeForeignKeyEnforcesInsertAndMatchSimpleNulls) {
    Parser parser;
    auto executor = makeTempExecutor("vertexdb-constraint-", "fk-composite-insert");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "CREATE TABLE Catalog (a INT, b INT, PRIMARY KEY (a, b));"))
                    .success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "CREATE TABLE Child (id INT PRIMARY KEY, a INT NULL, b INT NULL, "
                        "FOREIGN KEY (a, b) REFERENCES Catalog(a, b));"))
                    .success);
    ASSERT_TRUE(executor.execute(parser.parse("INSERT INTO Catalog VALUES (1, 2);")).success);
    ASSERT_TRUE(executor.execute(parser.parse("INSERT INTO Child VALUES (10, 1, 2);")).success);
    ASSERT_TRUE(executor.execute(parser.parse("INSERT INTO Child VALUES (11, 1, NULL);")).success);
    ASSERT_TRUE(executor.execute(parser.parse("INSERT INTO Child VALUES (12, NULL, 2);")).success);
    try {
        (void)executor.execute(parser.parse("INSERT INTO Child VALUES (13, 9, 9);"));
        FAIL() << "expected FOREIGN KEY rejection";
    } catch (const std::invalid_argument &ex) {
        EXPECT_NE(std::string{ex.what()}.find("FOREIGN KEY constraint violation on column a, b"),
                  std::string::npos);
    }
}

TEST(ConstraintBehaviorTests, CompositeForeignKeyOnDeleteCascadeAndSetNull) {
    Parser parser;
    auto executor = makeTempExecutor("vertexdb-constraint-", "fk-composite-actions");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "CREATE TABLE Catalog (a INT, b INT, PRIMARY KEY (a, b));"))
                    .success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "CREATE TABLE Cascaded (id INT PRIMARY KEY, a INT NULL, b INT NULL, "
                        "FOREIGN KEY (a, b) REFERENCES Catalog(a, b) ON DELETE CASCADE);"))
                    .success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "CREATE TABLE Nullable (id INT PRIMARY KEY, a INT NULL, b INT NULL, "
                        "FOREIGN KEY (a, b) REFERENCES Catalog(a, b) ON DELETE SET NULL);"))
                    .success);
    ASSERT_TRUE(executor.execute(parser.parse("INSERT INTO Catalog VALUES (1, 2), (3, 4);")).success);
    ASSERT_TRUE(executor.execute(parser.parse("INSERT INTO Cascaded VALUES (10, 1, 2);")).success);
    ASSERT_TRUE(executor.execute(parser.parse("INSERT INTO Nullable VALUES (20, 3, 4);")).success);

    ASSERT_TRUE(executor.execute(parser.parse("DELETE FROM Catalog WHERE a = 1;")).success);
    auto cascaded = executor.execute(parser.parse("SELECT id FROM Cascaded;"));
    ASSERT_TRUE(cascaded.success);
    EXPECT_TRUE(cascaded.rows.empty());

    ASSERT_TRUE(executor.execute(parser.parse("DELETE FROM Catalog WHERE a = 3;")).success);
    auto nullable = executor.execute(parser.parse("SELECT a, b FROM Nullable WHERE id = 20;"));
    ASSERT_TRUE(nullable.success);
    ASSERT_EQ(nullable.rows.size(), 1U);
    EXPECT_TRUE(nullable.rows[0][0].isNull());
    EXPECT_TRUE(nullable.rows[0][1].isNull());
}

TEST(ConstraintBehaviorTests, CompositeForeignKeyOnUpdateCascadePropagatesMappedColumn) {
    Parser parser;
    auto executor = makeTempExecutor("vertexdb-constraint-", "fk-composite-update");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "CREATE TABLE Catalog (a INT, b INT, PRIMARY KEY (a, b));"))
                    .success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "CREATE TABLE Child (id INT PRIMARY KEY, a INT NULL, b INT NULL, "
                        "FOREIGN KEY (a, b) REFERENCES Catalog(a, b) ON UPDATE CASCADE);"))
                    .success);
    ASSERT_TRUE(executor.execute(parser.parse("INSERT INTO Catalog VALUES (1, 2);")).success);
    ASSERT_TRUE(executor.execute(parser.parse("INSERT INTO Child VALUES (10, 1, 2);")).success);
    ASSERT_TRUE(executor.execute(parser.parse("UPDATE Catalog SET a = 9 WHERE a = 1;")).success);
    auto rows = executor.execute(parser.parse("SELECT a, b FROM Child WHERE id = 10;"));
    ASSERT_TRUE(rows.success);
    ASSERT_EQ(rows.rows.size(), 1U);
    EXPECT_EQ(rows.rows[0][0], Value{9});
    EXPECT_EQ(rows.rows[0][1], Value{2});
}

TEST(ConstraintBehaviorTests, SaveLoadPreservesCompositeForeignKeys) {
    Parser parser;
    auto executor = makeTempExecutor("vertexdb-constraint-", "fk-composite-save");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "CREATE TABLE Catalog (a INT, b INT, PRIMARY KEY (a, b));"))
                    .success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "CREATE TABLE Child (id INT PRIMARY KEY, a INT NULL, b INT NULL, "
                        "FOREIGN KEY (a, b) REFERENCES Catalog(a, b) ON DELETE CASCADE);"))
                    .success);
    ASSERT_TRUE(executor.execute(parser.parse("INSERT INTO Catalog VALUES (1, 2);")).success);
    ASSERT_TRUE(executor.execute(parser.parse("INSERT INTO Child VALUES (10, 1, 2);")).success);
    ASSERT_TRUE(executor.execute(parser.parse("SAVE DATABASE;")).success);
    ASSERT_TRUE(executor.execute(parser.parse("LOAD DATABASE;")).success);

    auto table = executor.currentDatabase()->table("Child");
    ASSERT_TRUE(table);
    ASSERT_EQ(table->foreignKeys().size(), 1U);
    ASSERT_EQ(table->foreignKeys()[0].childColumns.size(), 2U);
    EXPECT_EQ(table->foreignKeys()[0].onDelete, ForeignKeyAction::Cascade);

    ASSERT_TRUE(executor.execute(parser.parse("DELETE FROM Catalog WHERE a = 1;")).success);
    auto rows = executor.execute(parser.parse("SELECT id FROM Child;"));
    ASSERT_TRUE(rows.success);
    EXPECT_TRUE(rows.rows.empty());
}

TEST(ConstraintBehaviorTests, DropColumnRejectedWhenIndexed) {
    Parser parser;
    auto executor = makeTempExecutor("vertexdb-alter-drop-indexed-", "case");
    seedEmployees(executor, parser, true, false);

    auto result = executor.execute(parser.parse("ALTER TABLE Employees DROP COLUMN id;"));
    EXPECT_FALSE(result.success);
    EXPECT_NE(result.message.find("indexed"), std::string::npos);
}

TEST(ConstraintBehaviorTests, DropColumnRejectedWhenPrimaryKey) {
    Parser parser;
    auto executor = makeTempExecutor("vertexdb-alter-drop-pk-", "case");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(executor.execute(parser.parse("CREATE TABLE Accounts (id INT PRIMARY KEY, note STRING NULL);"))
                    .success);
    auto result = executor.execute(parser.parse("ALTER TABLE Accounts DROP COLUMN id;"));
    EXPECT_FALSE(result.success);
    EXPECT_NE(result.message.find("PRIMARY KEY"), std::string::npos);
}

TEST(ConstraintBehaviorTests, DropColumnRejectedWhenUnique) {
    Parser parser;
    auto executor = makeTempExecutor("vertexdb-alter-drop-uq-", "case");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "CREATE TABLE Accounts (id INT PRIMARY KEY, email STRING UNIQUE, "
                        "note STRING NULL);"))
                    .success);
    auto result = executor.execute(parser.parse("ALTER TABLE Accounts DROP COLUMN email;"));
    EXPECT_FALSE(result.success);
    EXPECT_NE(result.message.find("PRIMARY KEY or UNIQUE"), std::string::npos);
}

TEST(ConstraintBehaviorTests, DropColumnRejectedWhenCompositeUniqueMember) {
    Parser parser;
    auto executor = makeTempExecutor("vertexdb-alter-drop-comp-uq-", "case");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "CREATE TABLE Tags (id INT PRIMARY KEY, a INT NULL, b INT NULL, "
                        "UNIQUE (a, b));"))
                    .success);
    auto result = executor.execute(parser.parse("ALTER TABLE Tags DROP COLUMN a;"));
    EXPECT_FALSE(result.success);
    EXPECT_NE(result.message.find("part of a UNIQUE"), std::string::npos);
}

TEST(ConstraintBehaviorTests, DropLastColumnRejected) {
    Parser parser;
    auto executor = makeTempExecutor("vertexdb-alter-drop-last-", "case");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(executor.execute(parser.parse("CREATE TABLE Solo (note STRING NULL);")).success);
    auto result = executor.execute(parser.parse("ALTER TABLE Solo DROP COLUMN note;"));
    EXPECT_FALSE(result.success);
    EXPECT_NE(result.message.find("last column"), std::string::npos);
}

TEST(ConstraintBehaviorTests, DropColumnRejectedWhenCheckReferencesColumn) {
    Parser parser;
    auto executor = makeTempExecutor("vertexdb-alter-drop-check-", "case");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "CREATE TABLE Pay (id INT, salary DOUBLE, CHECK (salary > 0.0));"))
                    .success);
    auto result = executor.execute(parser.parse("ALTER TABLE Pay DROP COLUMN salary;"));
    EXPECT_FALSE(result.success);
    EXPECT_NE(result.message.find("CHECK"), std::string::npos);
}

TEST(ConstraintBehaviorTests, DropColumnRejectedWhenForeignKeyChild) {
    Parser parser;
    auto executor = makeTempExecutor("vertexdb-alter-drop-fk-child-", "case");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse("CREATE TABLE Customers (id INT PRIMARY KEY);"))
                    .success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "CREATE TABLE Orders (id INT PRIMARY KEY, customer_id INT "
                        "REFERENCES Customers(id));"))
                    .success);
    auto result = executor.execute(parser.parse("ALTER TABLE Orders DROP COLUMN customer_id;"));
    EXPECT_FALSE(result.success);
    EXPECT_NE(result.message.find("FOREIGN KEY"), std::string::npos);
}

TEST(ConstraintBehaviorTests, DropColumnRejectedWhenForeignKeyParent) {
    Parser parser;
    auto executor = makeTempExecutor("vertexdb-alter-drop-fk-parent-", "case");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse("CREATE TABLE Customers (id INT PRIMARY KEY, name STRING NULL);"))
                    .success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "CREATE TABLE Orders (id INT PRIMARY KEY, customer_id INT "
                        "REFERENCES Customers(id));"))
                    .success);
    auto result = executor.execute(parser.parse("ALTER TABLE Customers DROP COLUMN id;"));
    EXPECT_FALSE(result.success);
    EXPECT_NE(result.message.find("FOREIGN KEY"), std::string::npos);
}

TEST(ConstraintBehaviorTests, DropUnreferencedNullableColumnSucceeds) {
    Parser parser;
    auto executor = makeTempExecutor("vertexdb-alter-drop-ok-", "case");
    seedEmployees(executor, parser, false, false);
    ASSERT_TRUE(
        executor.execute(parser.parse("ALTER TABLE Employees ADD COLUMN note STRING NULL;"))
            .success);
    ASSERT_TRUE(executor.execute(parser.parse("ALTER TABLE Employees DROP COLUMN note;")).success);
    EXPECT_THROW((void)executor.execute(parser.parse("SELECT note FROM Employees WHERE id = 1;")),
                 std::runtime_error);
}

// Desired: ADD COLUMN DEFAULT pads existing rows; NOT NULL without DEFAULT rejects nonempty tables.
TEST(ConstraintBehaviorTests, AddColumnDefaultPadsExistingRows) {
    Parser parser;
    auto executor = makeTempExecutor("vertexdb-alter-add-default-", "case");
    seedEmployees(executor, parser, false, false);

    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "ALTER TABLE Employees ADD COLUMN bonus INT NULL DEFAULT 42;"))
                    .success);
    auto rows = executor.execute(parser.parse("SELECT bonus FROM Employees WHERE id = 1;"));
    ASSERT_TRUE(rows.success);
    ASSERT_EQ(rows.rows.size(), 1U);
    EXPECT_EQ(rows.rows[0][0], Value{static_cast<std::int64_t>(42)});
}

TEST(ConstraintBehaviorTests, AddColumnNotNullRequiresDefaultWhenTableHasRows) {
    Parser parser;
    auto executor = makeTempExecutor("vertexdb-alter-add-nn-", "case");
    seedEmployees(executor, parser, false, false);

    auto rejected =
        executor.execute(parser.parse("ALTER TABLE Employees ADD COLUMN flag INT NOT NULL;"));
    EXPECT_FALSE(rejected.success);
    EXPECT_NE(rejected.message.find("DEFAULT"), std::string::npos);

    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "ALTER TABLE Employees ADD COLUMN flag INT NOT NULL DEFAULT 1;"))
                    .success);
    try {
        (void)executor.execute(
            parser.parse("INSERT INTO Employees VALUES (99, \"Zed\", 1.0, NULL);"));
        FAIL() << "expected NOT NULL violation";
    } catch (const std::invalid_argument &ex) {
        EXPECT_NE(std::string{ex.what()}.find("NOT NULL constraint violation on column flag"),
                  std::string::npos);
    }
}

TEST(ConstraintBehaviorTests, AddColumnNotNullWithoutDefaultOnEmptyTableSucceeds) {
    Parser parser;
    auto executor = makeTempExecutor("vertexdb-alter-add-nn-empty-", "case");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(executor.execute(parser.parse("CREATE TABLE Empty (id INT);")).success);
    ASSERT_TRUE(
        executor.execute(parser.parse("ALTER TABLE Empty ADD COLUMN flag INT NOT NULL;")).success);
    auto table = executor.currentDatabase()->table("Empty");
    ASSERT_NE(table, nullptr);
    ASSERT_EQ(table->schema().size(), 2U);
    EXPECT_FALSE(table->schema()[1].nullable);
}

// Desired: DROP CASCADE removes same-table dependents; FK parent still rejected.
TEST(ConstraintBehaviorTests, DropColumnCascadeRemovesSameTableDependents) {
    Parser parser;
    auto executor = makeTempExecutor("vertexdb-alter-drop-cascade-", "case");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "CREATE TABLE Customers (id INT PRIMARY KEY, name STRING NULL);"))
                    .success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "CREATE TABLE Orders (id INT PRIMARY KEY, customer_id INT NULL "
                        "REFERENCES Customers(id), note STRING NULL, "
                        "CHECK (customer_id > 0));"))
                    .success);
    ASSERT_TRUE(
        executor.execute(parser.parse("CREATE INDEX idx_note ON Orders(note);")).success);

    auto plain =
        executor.execute(parser.parse("ALTER TABLE Orders DROP COLUMN customer_id;"));
    EXPECT_FALSE(plain.success);

    // Parent key still referenced — CASCADE on the parent must still reject.
    auto parentReject =
        executor.execute(parser.parse("ALTER TABLE Customers DROP COLUMN id CASCADE;"));
    EXPECT_FALSE(parentReject.success);
    EXPECT_NE(parentReject.message.find("FOREIGN KEY"), std::string::npos);

    ASSERT_TRUE(executor
                    .execute(parser.parse("ALTER TABLE Orders DROP COLUMN customer_id CASCADE;"))
                    .success);
    auto orders = executor.currentDatabase()->table("Orders");
    ASSERT_NE(orders, nullptr);
    EXPECT_FALSE(orders->columnIndex("customer_id").has_value());
    EXPECT_TRUE(orders->foreignKeys().empty());
    EXPECT_TRUE(orders->checkConstraints().empty());

    ASSERT_TRUE(
        executor.execute(parser.parse("ALTER TABLE Orders DROP COLUMN note CASCADE;")).success);
    EXPECT_FALSE(orders->hasIndex("note"));
}

TEST(ConstraintBehaviorTests, DropColumnCascadeRemovesUniqueConstraint) {
    Parser parser;
    auto executor = makeTempExecutor("vertexdb-alter-drop-cascade-uq-", "case");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "CREATE TABLE Accounts (id INT PRIMARY KEY, email STRING UNIQUE, "
                        "note STRING NULL);"))
                    .success);
    ASSERT_TRUE(
        executor.execute(parser.parse("ALTER TABLE Accounts DROP COLUMN email CASCADE;")).success);
    auto table = executor.currentDatabase()->table("Accounts");
    ASSERT_NE(table, nullptr);
    EXPECT_FALSE(table->columnIndex("email").has_value());
    EXPECT_TRUE(table->listIndexes().size() >= 1U); // __pk_id remains
}

// Desired: RENAME COLUMN rewrites schema, indexes, CHECKs, and parent FK names.
TEST(ConstraintBehaviorTests, RenameColumnRewritesDependentsAndParentForeignKeys) {
    Parser parser;
    auto executor = makeTempExecutor("vertexdb-alter-rename-", "case");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "CREATE TABLE Customers (id INT PRIMARY KEY, name STRING NULL);"))
                    .success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "CREATE TABLE Orders (id INT PRIMARY KEY, customer_id INT "
                        "REFERENCES Customers(id), amount DOUBLE, CHECK (amount > 0.0));"))
                    .success);
    ASSERT_TRUE(
        executor.execute(parser.parse("CREATE INDEX idx_amount ON Orders(amount);")).success);
    ASSERT_TRUE(executor.execute(parser.parse("INSERT INTO Customers VALUES (1, \"A\");")).success);
    ASSERT_TRUE(
        executor.execute(parser.parse("INSERT INTO Orders VALUES (10, 1, 5.0);")).success);

    ASSERT_TRUE(executor
                    .execute(parser.parse("ALTER TABLE Orders RENAME COLUMN amount TO total;"))
                    .success);
    auto renamed = executor.execute(parser.parse("SELECT total FROM Orders WHERE id = 10;"));
    ASSERT_TRUE(renamed.success);
    ASSERT_EQ(renamed.rows.size(), 1U);
    EXPECT_EQ(renamed.rows[0][0], Value{5.0});
    EXPECT_TRUE(executor.currentDatabase()->table("Orders")->hasIndex("total"));

    ASSERT_TRUE(executor
                    .execute(parser.parse("ALTER TABLE Customers RENAME COLUMN id TO cust_id;"))
                    .success);
    auto orders = executor.currentDatabase()->table("Orders");
    ASSERT_EQ(orders->foreignKeys().size(), 1U);
    EXPECT_EQ(orders->foreignKeys()[0].parentColumns.front(), "cust_id");
    ASSERT_TRUE(
        executor.execute(parser.parse("INSERT INTO Orders VALUES (11, 1, 6.0);")).success);
}

TEST(ConstraintBehaviorTests, RenameColumnRollbackRestoresOldName) {
    Parser parser;
    auto executor = makeTempExecutor("vertexdb-alter-rename-rb-", "case");
    seedEmployees(executor, parser, false, false);
    ASSERT_TRUE(executor.execute(parser.parse("BEGIN;")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse("ALTER TABLE Employees RENAME COLUMN name TO full_name;"))
                    .success);
    ASSERT_TRUE(executor.execute(parser.parse("ROLLBACK;")).success);
    auto rows = executor.execute(parser.parse("SELECT name FROM Employees WHERE id = 1;"));
    ASSERT_TRUE(rows.success);
    ASSERT_EQ(rows.rows.size(), 1U);
}

TEST(ConstraintBehaviorTests, RenameColumnCommitRecoversFromWal) {
    Parser parser;
    const auto root = makeTempRoot("vertexdb-alter-rename-wal-", "case");
    std::filesystem::remove_all(root);
    {
        QueryExecutor executor{root};
        ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
        ASSERT_TRUE(executor.execute(parser.parse("CREATE TABLE T (id INT, note STRING NULL);"))
                        .success);
        ASSERT_TRUE(executor.execute(parser.parse("INSERT INTO T VALUES (1, \"a\");")).success);
        ASSERT_TRUE(executor.execute(parser.parse("BEGIN;")).success);
        ASSERT_TRUE(
            executor.execute(parser.parse("ALTER TABLE T RENAME COLUMN note TO memo;")).success);
        ASSERT_TRUE(executor.execute(parser.parse("COMMIT;")).success);
    }
    QueryExecutor recovered{root};
    auto rows = recovered.execute(parser.parse("SELECT memo FROM T WHERE id = 1;"));
    ASSERT_TRUE(rows.success);
    ASSERT_EQ(rows.rows.size(), 1U);
    EXPECT_EQ(rows.rows[0][0], Value{std::string{"a"}});
    std::filesystem::remove_all(root);
}

TEST(ConstraintBehaviorTests, DropColumnCascadeRollbackRestoresDependents) {
    Parser parser;
    auto executor = makeTempExecutor("vertexdb-alter-drop-cascade-rb-", "case");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "CREATE TABLE Pay (id INT, salary DOUBLE, CHECK (salary > 0.0));"))
                    .success);
    ASSERT_TRUE(executor.execute(parser.parse("INSERT INTO Pay VALUES (1, 10.0);")).success);
    ASSERT_TRUE(executor.execute(parser.parse("BEGIN;")).success);
    ASSERT_TRUE(
        executor.execute(parser.parse("ALTER TABLE Pay DROP COLUMN salary CASCADE;")).success);
    ASSERT_TRUE(executor.execute(parser.parse("ROLLBACK;")).success);
    auto table = executor.currentDatabase()->table("Pay");
    ASSERT_NE(table, nullptr);
    EXPECT_TRUE(table->columnIndex("salary").has_value());
    ASSERT_EQ(table->checkConstraints().size(), 1U);
    auto rows = executor.execute(parser.parse("SELECT salary FROM Pay WHERE id = 1;"));
    ASSERT_TRUE(rows.success);
    ASSERT_EQ(rows.rows.size(), 1U);
    EXPECT_EQ(rows.rows[0][0], Value{10.0});
}

} // namespace VertexDB
