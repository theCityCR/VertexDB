#include "VertexDB/execution/query_executor.hpp"
#include "VertexDB/parser/parser.hpp"
#include "VertexDB/planner/query_planner.hpp"
#include "VertexDB/planner/rewriter.hpp"

#include <gtest/gtest.h>

#include <filesystem>

namespace VertexDB {

namespace {

QueryExecutor makeExecutor() {
    const auto root =
        std::filesystem::temp_directory_path() / "vertexdb-nested-sql-tests";
    std::filesystem::remove_all(root);
    return QueryExecutor{root};
}

} // namespace

TEST(NestedSqlTests, ParsesWithAndInSubqueryAndExplain) {
    Parser parser;

    auto withQuery = parser.parse(
        "WITH high AS (SELECT id, name, salary FROM Employees WHERE salary > 100000.0) "
        "SELECT name FROM high WHERE id = 1;");
    ASSERT_TRUE(std::holds_alternative<Select>(withQuery));
    const auto &withSelect = std::get<Select>(withQuery);
    ASSERT_EQ(withSelect.ctes.size(), 1U);
    EXPECT_EQ(withSelect.ctes[0].first, "high");
    EXPECT_EQ(withSelect.table, "high");
    ASSERT_TRUE(withSelect.where.has_value());
    EXPECT_EQ(withSelect.where->kind, Predicate::Kind::Comparison);

    auto inQuery =
        parser.parse("SELECT name FROM Employees WHERE id IN (SELECT id FROM Employees WHERE "
                     "salary > 100000.0);");
    ASSERT_TRUE(std::holds_alternative<Select>(inQuery));
    const auto &inSelect = std::get<Select>(inQuery);
    ASSERT_TRUE(inSelect.where.has_value());
    EXPECT_EQ(inSelect.where->kind, Predicate::Kind::InSubquery);
    ASSERT_TRUE(inSelect.where->subquery);
    EXPECT_EQ(inSelect.where->subquery->table, "Employees");

    auto explain = parser.parse("EXPLAIN SELECT name FROM Employees WHERE id = 1;");
    ASSERT_TRUE(std::holds_alternative<ExplainQuery>(explain));
    EXPECT_EQ(std::get<ExplainQuery>(explain).query.table, "Employees");
}

TEST(NestedSqlTests, RewriterInlinesCteAndPushesPredicates) {
    Parser parser;
    auto query = parser.parse(
        "WITH high AS (SELECT id, name, salary FROM Employees WHERE salary > 100000.0) "
        "SELECT name FROM high WHERE id = 1;");
    const auto &select = std::get<Select>(query);
    const auto rewritten = rewriteSelect(select);
    EXPECT_EQ(rewritten.query.table, "Employees");
    ASSERT_TRUE(rewritten.query.where.has_value());
    EXPECT_EQ(rewritten.query.where->kind, Predicate::Kind::And);
    ASSERT_FALSE(rewritten.notes.empty());
    EXPECT_NE(rewritten.notes.front().find("inlined CTE"), std::string::npos);
}

TEST(NestedSqlTests, ExplainShowsHashIndexAndResidualForAndPredicate) {
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

    auto explain = executor.execute(
        parser.parse("EXPLAIN SELECT name FROM Employees WHERE id = 1 AND salary > 100000.0;"));
    ASSERT_TRUE(explain.success);
    ASSERT_FALSE(explain.rows.empty());
    const auto text = explain.rows.front().front().toString();
    EXPECT_NE(text.find("hash index equality lookup"), std::string::npos);
    EXPECT_NE(text.find("residual: yes"), std::string::npos);

    auto result = executor.execute(
        parser.parse("SELECT name FROM Employees WHERE id = 1 AND salary > 100000.0;"));
    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.rows.size(), 1U);
    EXPECT_EQ(result.rows[0][0], Value{"Alice"});
}

TEST(NestedSqlTests, CteInliningUsesBaseTableIndex) {
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

    auto explain = executor.execute(parser.parse(
        "EXPLAIN WITH high AS (SELECT id, name, salary FROM Employees WHERE salary > 100000.0) "
        "SELECT name FROM high WHERE id = 1;"));
    ASSERT_TRUE(explain.success);
    ASSERT_FALSE(explain.rows.empty());
    const auto text = explain.rows.front().front().toString();
    EXPECT_NE(text.find("hash index equality lookup"), std::string::npos);
    EXPECT_NE(text.find("inlined CTE"), std::string::npos);

    auto result = executor.execute(parser.parse(
        "WITH high AS (SELECT id, name, salary FROM Employees WHERE salary > 100000.0) "
        "SELECT name FROM high WHERE id = 1;"));
    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.rows.size(), 1U);
    EXPECT_EQ(result.rows[0][0], Value{"Alice"});
}

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

TEST(NestedSqlTests, ParsesDerivedTableAsSyntheticCte) {
    Parser parser;
    auto query = parser.parse(
        "SELECT name FROM (SELECT id, name, salary FROM Employees WHERE salary > 100000.0) AS high "
        "WHERE id = 1;");
    ASSERT_TRUE(std::holds_alternative<Select>(query));
    const auto &select = std::get<Select>(query);
    EXPECT_EQ(select.table, "high");
    ASSERT_EQ(select.ctes.size(), 1U);
    EXPECT_EQ(select.ctes[0].first, "high");
    ASSERT_TRUE(select.ctes[0].second);
    EXPECT_EQ(select.ctes[0].second->table, "Employees");
}

TEST(NestedSqlTests, DerivedTableInlinesLikeCte) {
    Parser parser;
    auto query = parser.parse(
        "SELECT name FROM (SELECT id, name, salary FROM Employees WHERE salary > 100000.0) high "
        "WHERE id = 1;");
    const auto rewritten = rewriteSelect(std::get<Select>(query));
    EXPECT_EQ(rewritten.query.table, "Employees");
    ASSERT_TRUE(rewritten.query.where.has_value());
    EXPECT_EQ(rewritten.query.where->kind, Predicate::Kind::And);
    ASSERT_FALSE(rewritten.notes.empty());
    EXPECT_NE(rewritten.notes.front().find("inlined CTE high"), std::string::npos);
}

TEST(NestedSqlTests, DerivedTableUsesBaseTableIndex) {
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

    auto explain = executor.execute(parser.parse(
        "EXPLAIN SELECT name FROM (SELECT id, name, salary FROM Employees WHERE salary > 100000.0) "
        "AS high WHERE id = 1;"));
    ASSERT_TRUE(explain.success);
    const auto text = explain.rows.front().front().toString();
    EXPECT_NE(text.find("hash index equality lookup"), std::string::npos);
    EXPECT_NE(text.find("inlined CTE"), std::string::npos);

    auto result = executor.execute(parser.parse(
        "SELECT name FROM (SELECT id, name, salary FROM Employees WHERE salary > 100000.0) AS high "
        "WHERE id = 1;"));
    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.rows.size(), 1U);
    EXPECT_EQ(result.rows[0][0], Value{"Alice"});
}

TEST(NestedSqlTests, CteBodyJoinInlinesToJoinPlan) {
    Parser parser;
    auto executor = makeExecutor();
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(
        executor
            .execute(parser.parse("CREATE TABLE Employees (id INT, name STRING, dept_id INT);"))
            .success);
    ASSERT_TRUE(
        executor.execute(parser.parse("CREATE TABLE Departments (id INT, dept STRING);")).success);
    ASSERT_TRUE(executor.execute(parser.parse("INSERT INTO Employees VALUES (1, \"Alice\", 10);"))
                    .success);
    ASSERT_TRUE(executor.execute(parser.parse("INSERT INTO Departments VALUES (10, \"Eng\");"))
                    .success);

    auto result = executor.execute(parser.parse(
        "SELECT Employees.name FROM (SELECT * FROM Employees JOIN Departments ON dept_id = id) AS "
        "j;"));
    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.rows.size(), 1U);
    EXPECT_EQ(result.rows[0][0], Value{std::string{"Alice"}});
}

TEST(NestedSqlTests, PlannerChoosesHashIndexInLookup) {
    Table table{"Employees", {{"id", ColumnType::Int}, {"name", ColumnType::String}}};
    table.insert({Value{1}, Value{"Alice"}});
    table.insert({Value{2}, Value{"Bob"}});
    table.insert({Value{3}, Value{"Cara"}});
    ASSERT_TRUE(table.createIndex("idx_id", "id"));

    Select query{"Employees",
                 std::nullopt,
                 {"*"},
                 Predicate{"id", std::vector<Value>{Value{1}, Value{3}}},
                 {},
                 {}};
    QueryPlanner planner;
    const auto plan = planner.planSelect(query, table);
    EXPECT_EQ(plan.accessPath, AccessPath::HashIndexInLookup);
    EXPECT_EQ(plan.indexValues.size(), 2U);
}

} // namespace VertexDB
