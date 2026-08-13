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

#include <cstdint>
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
    EXPECT_EQ(table.foreignKeys[0].childColumn, "customer_id");
    EXPECT_EQ(table.foreignKeys[0].parentTable, "Customers");
    EXPECT_EQ(table.foreignKeys[0].parentColumn, "id");
    EXPECT_EQ(table.foreignKeys[1].childColumn, "customer_id");
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
    fk.onDelete = static_cast<ForeignKeyAction>(1);
    std::vector<ForeignKeyConstraint> badAction{fk};
    EXPECT_THROW(validateForeignKeyDefinitions(database, "Orders", childSchema, badAction),
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

} // namespace VertexDB
