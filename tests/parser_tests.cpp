#include "VertexDB/parser/parser.hpp"

#include <gtest/gtest.h>

namespace VertexDB {

TEST(ParserTests, ParsesCreateTable) {
    Parser parser;
    auto query = parser.parse("CREATE TABLE Employees (id INT, name STRING, salary DOUBLE);");

    ASSERT_TRUE(std::holds_alternative<CreateTable>(query));
    const auto &command = std::get<CreateTable>(query);
    EXPECT_EQ(command.name, "Employees");
    ASSERT_EQ(command.columns.size(), 3U);
    EXPECT_EQ(command.columns[0].type, ColumnType::Int);
    EXPECT_EQ(command.columns[1].type, ColumnType::String);
    EXPECT_EQ(command.columns[2].type, ColumnType::Double);
}

TEST(ParserTests, ParsesSelectWithPredicateAndLimit) {
    Parser parser;
    auto query = parser.parse("SELECT name FROM Employees WHERE salary > 100000.0 LIMIT 5;");

    ASSERT_TRUE(std::holds_alternative<Select>(query));
    const auto &command = std::get<Select>(query);
    EXPECT_EQ(command.table, "Employees");
    ASSERT_EQ(command.columns.size(), 1U);
    EXPECT_EQ(command.columns.front(), "name");
    ASSERT_TRUE(command.where.has_value());
    EXPECT_EQ(command.where->column, "salary");
    ASSERT_TRUE(command.limit.has_value());
    EXPECT_EQ(*command.limit, 5U);
}

TEST(ParserTests, ParsesMultiRowInsertNullableColumnsAndCompoundPredicates) {
    Parser parser;

    auto create = parser.parse("CREATE TABLE People (id INT, nickname STRING NULL);");
    ASSERT_TRUE(std::holds_alternative<CreateTable>(create));
    const auto &table = std::get<CreateTable>(create);
    ASSERT_EQ(table.columns.size(), 2U);
    EXPECT_TRUE(table.columns[1].nullable);

    auto insert = parser.parse("INSERT INTO People VALUES (1, \"Al\"), (2, NULL);");
    ASSERT_TRUE(std::holds_alternative<Insert>(insert));
    const auto &insertCommand = std::get<Insert>(insert);
    ASSERT_EQ(insertCommand.rows.size(), 2U);
    EXPECT_TRUE(insertCommand.rows[1][1].isNull());

    auto select = parser.parse("SELECT * FROM People WHERE id = 1 OR (id > 2 AND id < 5);");
    ASSERT_TRUE(std::holds_alternative<Select>(select));
    const auto &selectCommand = std::get<Select>(select);
    ASSERT_TRUE(selectCommand.where.has_value());
    EXPECT_EQ(selectCommand.where->kind, Predicate::Kind::Or);
}

TEST(ParserTests, ParsesOrderByAndTransactionCommands) {
    Parser parser;

    auto select = parser.parse("SELECT * FROM Employees ORDER BY salary DESC LIMIT 2;");
    ASSERT_TRUE(std::holds_alternative<Select>(select));
    const auto &command = std::get<Select>(select);
    ASSERT_TRUE(command.orderBy.has_value());
    EXPECT_EQ(command.orderBy->column, "salary");
    EXPECT_FALSE(command.orderBy->ascending);

    EXPECT_TRUE(std::holds_alternative<BeginTransaction>(parser.parse("BEGIN;")));
    EXPECT_TRUE(std::holds_alternative<CommitTransaction>(parser.parse("COMMIT;")));
    EXPECT_TRUE(std::holds_alternative<RollbackTransaction>(parser.parse("ROLLBACK;")));
}

TEST(ParserTests, ParsesTableManagementCommands) {
    Parser parser;

    EXPECT_TRUE(std::holds_alternative<DropTable>(parser.parse("DROP TABLE Employees;")));
    EXPECT_TRUE(
        std::holds_alternative<RenameTable>(parser.parse("RENAME TABLE Employees TO Staff;")));
    EXPECT_TRUE(std::holds_alternative<ListTables>(parser.parse("LIST TABLES;")));
}

TEST(ParserTests, RejectsTrailingTokens) {
    Parser parser;

    EXPECT_THROW((void)parser.parse("SELECT * FROM Employees unexpected;"), std::runtime_error);
}

TEST(ParserTests, ParsesJoinAndPreparedStatements) {
    Parser parser;

    auto join = parser.parse("SELECT * FROM Employees JOIN Departments ON dept_id = id LIMIT 5;");
    ASSERT_TRUE(std::holds_alternative<Select>(join));
    const auto &select = std::get<Select>(join);
    ASSERT_TRUE(select.join.has_value());
    EXPECT_EQ(select.join->table, "Departments");
    EXPECT_EQ(select.join->leftColumn, "dept_id");
    EXPECT_EQ(select.join->rightColumn, "id");

    auto qualified =
        parser.parse("SELECT Employees.name, Departments.dept FROM Employees JOIN Departments ON "
                     "Employees.dept_id = Departments.id;");
    ASSERT_TRUE(std::holds_alternative<Select>(qualified));
    const auto &qualifiedSelect = std::get<Select>(qualified);
    ASSERT_EQ(qualifiedSelect.columns.size(), 2U);
    EXPECT_EQ(qualifiedSelect.columns[0], "Employees.name");
    EXPECT_EQ(qualifiedSelect.join->leftColumn, "Employees.dept_id");

    EXPECT_TRUE(std::holds_alternative<PrepareStatement>(
        parser.parse("PREPARE by_id AS \"SELECT name FROM Employees WHERE id = ?;\";")));

    auto execute = parser.parse("EXECUTE by_id VALUES (1);");
    ASSERT_TRUE(std::holds_alternative<ExecutePrepared>(execute));
    EXPECT_EQ(std::get<ExecutePrepared>(execute).parameters.size(), 1U);
}

} // namespace VertexDB
