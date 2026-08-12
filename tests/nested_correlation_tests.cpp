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

TEST(NestedSqlTests, CorrelatedExistsAndInMatchOuterRow) {
    Parser parser;
    auto executor = makeExecutor();
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
        executor.execute(parser.parse("INSERT INTO Bonuses VALUES (1, 5000.0);")).success);

    auto existsResult = executor.execute(parser.parse(
        "SELECT name FROM Employees WHERE EXISTS (SELECT emp_id FROM Bonuses WHERE emp_id = "
        "Employees.id);"));
    ASSERT_TRUE(existsResult.success);
    ASSERT_EQ(existsResult.rows.size(), 1U);
    EXPECT_EQ(existsResult.rows[0][0], Value{"Alice"});

    auto inResult = executor.execute(parser.parse(
        "SELECT name FROM Employees WHERE id IN (SELECT emp_id FROM Bonuses WHERE emp_id = id);"));
    ASSERT_TRUE(inResult.success);
    ASSERT_EQ(inResult.rows.size(), 1U);
    EXPECT_EQ(inResult.rows[0][0], Value{"Alice"});
}

TEST(NestedSqlTests, CorrelatedInExistsWithNullOuterKey) {
    // Intentional: bound NULL outer keys compare equal to NULL via Value== (unlike SQL UNKNOWN).
    Parser parser;
    auto executor = makeExecutor("corr-null-outer");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(
        executor
            .execute(parser.parse("CREATE TABLE Employees (id INT NULL, name STRING);"))
            .success);
    ASSERT_TRUE(
        executor.execute(parser.parse("CREATE TABLE Bonuses (emp_id INT NULL, amount DOUBLE);"))
            .success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "INSERT INTO Employees VALUES (1, \"Alice\"), (NULL, \"NullEmp\"), "
                        "(2, \"Bob\");"))
                    .success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "INSERT INTO Bonuses VALUES (1, 5000.0), (NULL, 100.0);"))
                    .success);

    auto existsResult = executor.execute(parser.parse(
        "SELECT name FROM Employees WHERE EXISTS ("
        "  SELECT emp_id FROM Bonuses WHERE emp_id = Employees.id"
        ") ORDER BY name;"));
    ASSERT_TRUE(existsResult.success) << existsResult.message;
    ASSERT_EQ(existsResult.rows.size(), 2U);
    EXPECT_EQ(existsResult.rows[0][0], Value{"Alice"});
    EXPECT_EQ(existsResult.rows[1][0], Value{"NullEmp"});

    auto inResult = executor.execute(parser.parse(
        "SELECT name FROM Employees WHERE id IN ("
        "  SELECT emp_id FROM Bonuses WHERE emp_id = id"
        ") ORDER BY name;"));
    ASSERT_TRUE(inResult.success) << inResult.message;
    ASSERT_EQ(inResult.rows.size(), 2U);
    EXPECT_EQ(inResult.rows[0][0], Value{"Alice"});
    EXPECT_EQ(inResult.rows[1][0], Value{"NullEmp"});
}

TEST(NestedSqlTests, TwoLevelCorrelatedExistsBindsOutermost) {
    Parser parser;
    auto executor = makeExecutor("two-level-exists");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(
        executor
            .execute(parser.parse("CREATE TABLE Employees (id INT, name STRING, salary DOUBLE);"))
            .success);
    ASSERT_TRUE(executor.execute(parser.parse("CREATE TABLE Bonuses (emp_id INT, amount DOUBLE);"))
                    .success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "INSERT INTO Employees VALUES (1, \"Alice\", 120000.0), (2, \"Bob\", "
                        "90000.0);"))
                    .success);
    ASSERT_TRUE(
        executor.execute(parser.parse("INSERT INTO Bonuses VALUES (1, 5000.0), (2, 1000.0);"))
            .success);

    // Innermost correlates to outermost Employees through one mid-level EXISTS.
    auto result = executor.execute(parser.parse(
        "SELECT name FROM Employees WHERE EXISTS ("
        "  SELECT emp_id FROM Bonuses WHERE EXISTS ("
        "    SELECT emp_id FROM Bonuses WHERE emp_id = Employees.id"
        "  )"
        ") ORDER BY name;"));
    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.rows.size(), 2U);
    EXPECT_EQ(result.rows[0][0], Value{"Alice"});
    EXPECT_EQ(result.rows[1][0], Value{"Bob"});

    // Mid-level correlation: innermost refers to Bonuses, not the outermost Employees.
    auto mid = executor.execute(parser.parse(
        "SELECT name FROM Employees WHERE EXISTS ("
        "  SELECT emp_id FROM Bonuses WHERE emp_id = Employees.id AND amount > 2000.0 AND "
        "EXISTS ("
        "    SELECT id FROM Employees WHERE id = Bonuses.emp_id"
        "  )"
        ") ORDER BY name;"));
    ASSERT_TRUE(mid.success);
    ASSERT_EQ(mid.rows.size(), 1U);
    EXPECT_EQ(mid.rows[0][0], Value{"Alice"});
}

TEST(NestedSqlTests, TwoLevelCorrelatedInBindsOutermost) {
    Parser parser;
    auto executor = makeExecutor("two-level-in");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(
        executor
            .execute(parser.parse("CREATE TABLE Employees (id INT, name STRING, salary DOUBLE);"))
            .success);
    ASSERT_TRUE(executor.execute(parser.parse("CREATE TABLE Bonuses (emp_id INT, amount DOUBLE);"))
                    .success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "INSERT INTO Employees VALUES (1, \"Alice\", 120000.0), (2, \"Bob\", "
                        "90000.0);"))
                    .success);
    ASSERT_TRUE(
        executor.execute(parser.parse("INSERT INTO Bonuses VALUES (1, 5000.0), (2, 1000.0);"))
            .success);

    // Innermost correlates to outermost Employees through one mid-level IN.
    auto result = executor.execute(parser.parse(
        "SELECT name FROM Employees WHERE id IN ("
        "  SELECT emp_id FROM Bonuses WHERE emp_id IN ("
        "    SELECT emp_id FROM Bonuses WHERE emp_id = Employees.id"
        "  )"
        ") ORDER BY name;"));
    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.rows.size(), 2U);
    EXPECT_EQ(result.rows[0][0], Value{"Alice"});
    EXPECT_EQ(result.rows[1][0], Value{"Bob"});

    // Mid-level filter plus innermost correlation to outermost Employees.
    auto filtered = executor.execute(parser.parse(
        "SELECT name FROM Employees WHERE id IN ("
        "  SELECT emp_id FROM Bonuses WHERE amount > 2000.0 AND emp_id IN ("
        "    SELECT emp_id FROM Bonuses WHERE emp_id = Employees.id"
        "  )"
        ") ORDER BY name;"));
    ASSERT_TRUE(filtered.success);
    ASSERT_EQ(filtered.rows.size(), 1U);
    EXPECT_EQ(filtered.rows[0][0], Value{"Alice"});
}

TEST(NestedSqlTests, ThreeLevelCorrelationBindsOutermost) {
    Parser parser;
    auto executor = makeExecutor("three-level-exists");
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
    ASSERT_TRUE(executor
                    .execute(parser.parse("INSERT INTO Bonuses VALUES (1, 500.0), (1, 1500.0), "
                                          "(2, 100.0);"))
                    .success);

    auto result = executor.execute(parser.parse(
        "SELECT name FROM Employees WHERE EXISTS ("
        "  SELECT emp_id FROM Bonuses WHERE EXISTS ("
        "    SELECT emp_id FROM Bonuses WHERE EXISTS ("
        "      SELECT emp_id FROM Bonuses WHERE emp_id = Employees.id AND amount > 1000.0"
        "    )"
        "  )"
        ") ORDER BY name;"));
    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.rows.size(), 1U);
    EXPECT_EQ(result.rows[0][0], Value{"Alice"});
}

TEST(NestedSqlTests, FourLevelCorrelationBindsOutermost) {
    // Documented max: up to four outer FROM frames may correlate.
    Parser parser;
    auto executor = makeExecutor("four-level-exists");
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
    ASSERT_TRUE(executor
                    .execute(parser.parse("INSERT INTO Bonuses VALUES (1, 500.0), (1, 1500.0), "
                                          "(2, 100.0);"))
                    .success);

    auto result = executor.execute(parser.parse(
        "SELECT name FROM Employees WHERE EXISTS ("
        "  SELECT emp_id FROM Bonuses WHERE EXISTS ("
        "    SELECT emp_id FROM Bonuses WHERE EXISTS ("
        "      SELECT emp_id FROM Bonuses WHERE EXISTS ("
        "        SELECT emp_id FROM Bonuses WHERE emp_id = Employees.id AND amount > 1000.0"
        "      )"
        "    )"
        "  )"
        ") ORDER BY name;"));
    ASSERT_TRUE(result.success) << result.message;
    ASSERT_EQ(result.rows.size(), 1U);
    EXPECT_EQ(result.rows[0][0], Value{"Alice"});
}

TEST(NestedSqlTests, FiveLevelCorrelationIsRejected) {
    Parser parser;
    EXPECT_THROW(
        (void)parser.parse(
            "SELECT name FROM Employees WHERE EXISTS ("
            "  SELECT emp_id FROM Bonuses WHERE EXISTS ("
            "    SELECT emp_id FROM Bonuses WHERE EXISTS ("
            "      SELECT emp_id FROM Bonuses WHERE EXISTS ("
            "        SELECT emp_id FROM Bonuses WHERE EXISTS ("
            "          SELECT emp_id FROM Bonuses WHERE emp_id = Employees.id"
            "        )"
            "      )"
            "    )"
            "  )"
            ");"),
        std::runtime_error);
}

TEST(NestedSqlTests, CorrelatedExistsReturnsEmptyWhenNoMatch) {
    Parser parser;
    auto executor = makeExecutor();
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
    // Only Bob has a bonus; Alice should be filtered out by EXISTS.
    ASSERT_TRUE(
        executor.execute(parser.parse("INSERT INTO Bonuses VALUES (2, 1000.0);")).success);

    auto existsResult = executor.execute(parser.parse(
        "SELECT name FROM Employees WHERE EXISTS (SELECT emp_id FROM Bonuses WHERE emp_id = "
        "Employees.id) ORDER BY name;"));
    ASSERT_TRUE(existsResult.success);
    ASSERT_EQ(existsResult.rows.size(), 1U);
    EXPECT_EQ(existsResult.rows[0][0], Value{"Bob"});

    auto none = executor.execute(parser.parse(
        "SELECT name FROM Employees WHERE EXISTS (SELECT emp_id FROM Bonuses WHERE emp_id = 999);"));
    ASSERT_TRUE(none.success);
    EXPECT_TRUE(none.rows.empty());
}

TEST(NestedSqlTests, CorrelatedInBindsOuterColumnPerRow) {
    Parser parser;
    auto executor = makeExecutor("correlated-in");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(
        executor
            .execute(parser.parse("CREATE TABLE Employees (id INT, name STRING, salary DOUBLE);"))
            .success);
    ASSERT_TRUE(
        executor.execute(parser.parse("CREATE TABLE Peer (emp_id INT, label STRING);")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "INSERT INTO Employees VALUES (1, \"Alice\", 120000.0), (2, \"Bob\", "
                        "90000.0);"))
                    .success);
    ASSERT_TRUE(
        executor.execute(parser.parse("INSERT INTO Peer VALUES (1, \"a\"), (1, \"b\");")).success);

    auto result = executor.execute(parser.parse(
        "SELECT name FROM Employees WHERE id IN (SELECT emp_id FROM Peer WHERE emp_id = "
        "Employees.id);"));
    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.rows.size(), 1U);
    EXPECT_EQ(result.rows[0][0], Value{"Alice"});
}

TEST(NestedSqlTests, TableAliasParsesAndScopesCorrelation) {
    Parser parser;
    auto query = parser.parse(
        "SELECT e.name FROM Employees AS e WHERE EXISTS ("
        "SELECT emp_id FROM Bonuses AS b WHERE b.emp_id = e.id);");
    ASSERT_TRUE(std::holds_alternative<Select>(query));
    const auto &select = std::get<Select>(query);
    EXPECT_EQ(select.table, "Employees");
    ASSERT_TRUE(select.tableAlias.has_value());
    EXPECT_EQ(*select.tableAlias, "e");
}

TEST(NestedSqlTests, TableAliasCorrelatedInMatchesOuterRows) {
    Parser parser;
    auto executor = makeExecutor("table-alias-correlated-in");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(
        executor
            .execute(parser.parse("CREATE TABLE Employees (id INT, name STRING, salary DOUBLE);"))
            .success);
    ASSERT_TRUE(
        executor.execute(parser.parse("CREATE TABLE Peer (emp_id INT, label STRING);")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "INSERT INTO Employees VALUES (1, \"Alice\", 120000.0), (2, \"Bob\", "
                        "90000.0);"))
                    .success);
    ASSERT_TRUE(
        executor.execute(parser.parse("INSERT INTO Peer VALUES (1, \"a\"), (1, \"b\");")).success);

    auto result = executor.execute(parser.parse(
        "SELECT e.name FROM Employees e WHERE e.id IN ("
        "SELECT p.emp_id FROM Peer AS p WHERE p.emp_id = e.id) ORDER BY e.name;"));
    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.rows.size(), 1U);
    EXPECT_EQ(result.rows[0][0], Value{"Alice"});
}

TEST(NestedSqlTests, JoinTableAliasesQualifySelectAndOn) {
    Parser parser;
    auto executor = makeExecutor("join-table-aliases");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "CREATE TABLE Employees (id INT, name STRING, dept_id INT, salary DOUBLE);"))
                    .success);
    ASSERT_TRUE(
        executor.execute(parser.parse("CREATE TABLE Departments (id INT, dept STRING);")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "INSERT INTO Employees VALUES (1, \"Alice\", 10, 120000.0), "
                        "(2, \"Bob\", 20, 80000.0);"))
                    .success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "INSERT INTO Departments VALUES (10, \"Eng\"), (20, \"Sales\");"))
                    .success);

    auto parsed = parser.parse(
        "SELECT e.name, d.dept FROM Employees AS e JOIN Departments AS d "
        "ON e.dept_id = d.id WHERE e.salary > 100000.0;");
    ASSERT_TRUE(std::holds_alternative<Select>(parsed));
    const auto &select = std::get<Select>(parsed);
    ASSERT_TRUE(select.tableAlias.has_value());
    EXPECT_EQ(*select.tableAlias, "e");
    ASSERT_EQ(select.joins.size(), 1U);
    ASSERT_TRUE(select.joins[0].tableAlias.has_value());
    EXPECT_EQ(*select.joins[0].tableAlias, "d");

    auto result = executor.execute(parsed);
    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.rows.size(), 1U);
    EXPECT_EQ(result.rows[0][0], Value{"Alice"});
    EXPECT_EQ(result.rows[0][1], Value{"Eng"});
}

} // namespace VertexDB
