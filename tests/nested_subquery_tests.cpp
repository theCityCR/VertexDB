#include "test_support.hpp"

#include "VertexDB/execution/query_executor.hpp"
#include "VertexDB/execution/recursive_cte_limits.hpp"
#include "VertexDB/parser/parser.hpp"
#include "VertexDB/planner/query_planner.hpp"
#include "VertexDB/planner/rewriter.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

namespace VertexDB {

namespace {

QueryExecutor makeExecutor(std::string_view suffix) {
    return makeTempExecutor("vertexdb-nested-", suffix);
}

QueryExecutor makeExecutor() {
    return makeExecutor("nested-sql-tests");
}

} // namespace

TEST(NestedSqlTests, InSubqueryUsesIndexOnSubqueryAndOuterInLookup) {
    Parser parser;
    auto executor = makeExecutor();
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
    ASSERT_TRUE(
        executor.execute(parser.parse("CREATE INDEX idx_salary ON Employees(salary);")).success);

    auto explainSubquery = executor.execute(
        parser.parse("EXPLAIN SELECT id FROM Employees WHERE salary > 100000.0;"));
    ASSERT_TRUE(explainSubquery.success);
    EXPECT_NE(explainSubquery.rows.front().front().toString().find("ordered index range lookup"),
              std::string::npos);

    auto explainIn = executor.execute(parser.parse(
        "EXPLAIN SELECT name FROM Employees WHERE id IN (SELECT id FROM Employees WHERE salary > "
        "100000.0);"));
    ASSERT_TRUE(explainIn.success);
    const auto inPlan = explainIn.rows.front().front().toString();
    EXPECT_NE(inPlan.find("hash index IN lookup"), std::string::npos);

    auto result = executor.execute(parser.parse(
        "SELECT name FROM Employees WHERE id IN (SELECT id FROM Employees WHERE salary > "
        "100000.0) ORDER BY name ASC;"));
    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.rows.size(), 2U);
    EXPECT_EQ(result.rows[0][0], Value{"Alice"});
    EXPECT_EQ(result.rows[1][0], Value{"Cara"});
}

TEST(NestedSqlTests, PlannerChoosesHashIn) {
    Table table{"Employees", {{"id", ColumnType::Int}, {"name", ColumnType::String}}};
    table.insert({Value{1}, Value{"Alice"}});
    table.insert({Value{2}, Value{"Bob"}});
    table.insert({Value{3}, Value{"Cara"}});
    ASSERT_TRUE(table.createIndex("idx_id", "id"));

    Select query{"Employees",
                 {},
                 {SelectExpr::makeStar()},
                 makeInList("id", {Value{1}, Value{3}}),
                 {},
                 {}};
    QueryPlanner planner;
    const auto plan = planner.planSelect(query, table);
    EXPECT_EQ(plan.accessPath(), AccessPath::HashIn);
    EXPECT_EQ(std::get<HashInPlan>(plan.path).indexValues.size(), 2U);
}

TEST(NestedSqlTests, EmptyUncorrelatedInSubqueryMatchesNoRows) {
    Parser parser;
    auto executor = makeExecutor("empty-in-subquery");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(
        executor
            .execute(parser.parse("CREATE TABLE Employees (id INT, name STRING, salary DOUBLE);"))
            .success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "INSERT INTO Employees VALUES (1, \"Alice\", 120000.0), (2, \"Bob\", "
                        "90000.0);"))
                    .success);

    auto result = executor.execute(parser.parse(
        "SELECT name FROM Employees WHERE id IN (SELECT id FROM Employees WHERE salary > "
        "999999.0);"));
    ASSERT_TRUE(result.success) << result.message;
    EXPECT_TRUE(result.rows.empty());
}

TEST(NestedSqlTests, ExpressionIndexEqualityAndRangeExplain) {
    Parser parser;
    auto executor = makeExecutor();
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
    ASSERT_TRUE(
        executor.execute(parser.parse("CREATE INDEX idx_neg_salary ON Employees((-salary));"))
            .success);
    ASSERT_TRUE(
        executor.execute(parser.parse("CREATE INDEX idx_id_plus ON Employees((id+1));")).success);

    auto eqExplain = executor.execute(
        parser.parse("EXPLAIN SELECT name FROM Employees WHERE (-salary) = -120000.0;"));
    ASSERT_TRUE(eqExplain.success);
    EXPECT_NE(eqExplain.rows.front().front().toString().find("expression hash index"),
              std::string::npos);

    auto eqResult =
        executor.execute(parser.parse("SELECT name FROM Employees WHERE (-salary) = -120000.0;"));
    ASSERT_TRUE(eqResult.success);
    ASSERT_EQ(eqResult.rows.size(), 1U);
    EXPECT_EQ(eqResult.rows[0][0], Value{"Alice"});

    auto rangeExplain =
        executor.execute(parser.parse("EXPLAIN SELECT name FROM Employees WHERE (id+1) > 2;"));
    ASSERT_TRUE(rangeExplain.success);
    EXPECT_NE(rangeExplain.rows.front().front().toString().find("expression ordered index"),
              std::string::npos);
}

TEST(NestedSqlTests, NestedSqlDocumentedRefusalsAreRejected) {
    Parser parser;

    // Derived tables require an alias.
    EXPECT_THROW((void)parser.parse("SELECT id FROM (SELECT id FROM Employees);"),
                 std::runtime_error);

    // Derived-table syntax in the JOIN position is unsupported (use a CTE name instead).
    EXPECT_THROW((void)parser.parse(
                     "SELECT Employees.id FROM Employees JOIN (SELECT id FROM Departments) AS d "
                     "ON Employees.dept_id = d.id;"),
                 std::runtime_error);
}

TEST(NestedSqlTests, JoinInsideInSubqueryFiltersByJoinedKeys) {
    Parser parser;
    auto executor = makeExecutor("join-inside-in");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(
        executor.execute(parser.parse("CREATE TABLE Employees (id INT, name STRING, dept_id INT);"))
            .success);
    ASSERT_TRUE(
        executor.execute(parser.parse("CREATE TABLE Departments (id INT, dept STRING);")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse("INSERT INTO Employees VALUES (1, \"Alice\", 10), "
                                          "(2, \"Bob\", 99);"))
                    .success);
    ASSERT_TRUE(
        executor.execute(parser.parse("INSERT INTO Departments VALUES (10, \"Eng\");")).success);

    auto result = executor.execute(parser.parse(
        "SELECT name FROM Employees WHERE id IN ("
        "SELECT Employees.id FROM Employees JOIN Departments "
        "ON Employees.dept_id = Departments.id) ORDER BY name;"));
    ASSERT_TRUE(result.success) << result.message;
    ASSERT_EQ(result.rows.size(), 1U);
    EXPECT_EQ(result.rows[0][0], Value{"Alice"});
}

TEST(NestedSqlTests, JoinInsideExistsCorrelatesToOuterRow) {
    Parser parser;
    auto executor = makeExecutor("join-inside-exists");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(
        executor.execute(parser.parse("CREATE TABLE Employees (id INT, name STRING);")).success);
    ASSERT_TRUE(
        executor.execute(parser.parse("CREATE TABLE Assignments (emp_id INT, dept_id INT);"))
            .success);
    ASSERT_TRUE(
        executor.execute(parser.parse("CREATE TABLE Departments (id INT, dept STRING);")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse("INSERT INTO Employees VALUES (1, \"Alice\"), "
                                          "(2, \"Bob\");"))
                    .success);
    ASSERT_TRUE(executor
                    .execute(parser.parse("INSERT INTO Assignments VALUES (1, 10), (2, 99);"))
                    .success);
    ASSERT_TRUE(
        executor.execute(parser.parse("INSERT INTO Departments VALUES (10, \"Eng\");")).success);

    auto result = executor.execute(parser.parse(
        "SELECT e.name FROM Employees AS e WHERE EXISTS ("
        "SELECT Assignments.emp_id FROM Assignments JOIN Departments "
        "ON Assignments.dept_id = Departments.id WHERE Assignments.emp_id = e.id) "
        "ORDER BY e.name;"));
    ASSERT_TRUE(result.success) << result.message;
    ASSERT_EQ(result.rows.size(), 1U);
    EXPECT_EQ(result.rows[0][0], Value{"Alice"});
}

TEST(NestedSqlTests, WithInsideInSubqueryInlinesAndFilters) {
    Parser parser;
    auto executor = makeExecutor("with-inside-in");
    seedEmployees(executor, parser, true, false);

    auto result = executor.execute(parser.parse(
        "SELECT name FROM Employees WHERE id IN ("
        "WITH high AS (SELECT id FROM Employees WHERE salary > 100000.0 AND id = 1) "
        "SELECT id FROM high) ORDER BY name;"));
    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.rows.size(), 1U);
    EXPECT_EQ(result.rows[0][0], Value{"Alice"});
}

TEST(NestedSqlTests, WithInsideExistsCorrelatesThroughOuterAlias) {
    Parser parser;
    auto executor = makeExecutor("with-inside-exists-alias");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(
        executor
            .execute(parser.parse("CREATE TABLE Employees (id INT, name STRING, salary DOUBLE);"))
            .success);
    ASSERT_TRUE(
        executor.execute(parser.parse("CREATE TABLE Bonuses (emp_id INT, amount DOUBLE);")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "INSERT INTO Employees VALUES (1, \"Alice\", 120000.0), (2, \"Bob\", "
                        "90000.0);"))
                    .success);
    ASSERT_TRUE(
        executor.execute(parser.parse("INSERT INTO Bonuses VALUES (1, 5000.0), (2, 100.0);"))
            .success);

    auto result = executor.execute(parser.parse(
        "SELECT e.name FROM Employees AS e WHERE EXISTS ("
        "WITH b AS (SELECT emp_id, amount FROM Bonuses WHERE amount > 1000.0) "
        "SELECT emp_id FROM b WHERE emp_id = e.id) ORDER BY e.name;"));
    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.rows.size(), 1U);
    EXPECT_EQ(result.rows[0][0], Value{"Alice"});
}

TEST(NestedSqlTests, DerivedTableInsideInSubqueryIsAllowed) {
    Parser parser;
    auto executor = makeExecutor("derived-inside-in");
    seedEmployees(executor, parser, true, false);

    auto result = executor.execute(parser.parse(
        "SELECT name FROM Employees WHERE id IN ("
        "SELECT id FROM (SELECT id FROM Employees WHERE salary > 100000.0 AND id = 1) AS high);"));
    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.rows.size(), 1U);
    EXPECT_EQ(result.rows[0][0], Value{"Alice"});
}

TEST(NestedSqlTests, DerivedTableInsideExistsIsAllowed) {
    Parser parser;
    auto executor = makeExecutor("derived-inside-exists");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(
        executor
            .execute(parser.parse("CREATE TABLE Employees (id INT, name STRING, salary DOUBLE);"))
            .success);
    ASSERT_TRUE(
        executor.execute(parser.parse("CREATE TABLE Bonuses (emp_id INT, amount DOUBLE);")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "INSERT INTO Employees VALUES (1, \"Alice\", 120000.0), (2, \"Bob\", "
                        "90000.0);"))
                    .success);
    ASSERT_TRUE(
        executor.execute(parser.parse("INSERT INTO Bonuses VALUES (1, 5000.0), (2, 100.0);"))
            .success);

    auto result = executor.execute(parser.parse(
        "SELECT name FROM Employees WHERE EXISTS ("
        "SELECT emp_id FROM (SELECT emp_id, amount FROM Bonuses WHERE amount > 1000.0) AS b "
        "WHERE emp_id = Employees.id) ORDER BY name;"));
    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.rows.size(), 1U);
    EXPECT_EQ(result.rows[0][0], Value{"Alice"});
}

} // namespace VertexDB
