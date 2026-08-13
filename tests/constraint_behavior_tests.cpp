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

} // namespace VertexDB
