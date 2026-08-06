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
    EXPECT_EQ(withSelect.ctes[0].name, "high");
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
    EXPECT_EQ(select.ctes[0].name, "high");
    ASSERT_TRUE(select.ctes[0].body);
    EXPECT_EQ(select.ctes[0].body->table, "Employees");
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
                 {},
                 {SelectExpr::makeStar()},
                 Predicate{"id", std::vector<Value>{Value{1}, Value{3}}},
                 {},
                 {}};
    QueryPlanner planner;
    const auto plan = planner.planSelect(query, table);
    EXPECT_EQ(plan.accessPath, AccessPath::HashIndexInLookup);
    EXPECT_EQ(plan.indexValues.size(), 2U);
}

TEST(NestedSqlTests, ParsesMaterializedAndNotMaterializedCte) {
    Parser parser;
    auto materialized = parser.parse(
        "WITH high AS MATERIALIZED (SELECT id, name FROM Employees WHERE salary > 100000.0) "
        "SELECT name FROM high WHERE id = 1;");
    ASSERT_TRUE(std::holds_alternative<Select>(materialized));
    const auto &matSelect = std::get<Select>(materialized);
    ASSERT_EQ(matSelect.ctes.size(), 1U);
    EXPECT_EQ(matSelect.ctes[0].materializeMode, MaterializeMode::Materialized);

    auto notMat = parser.parse(
        "WITH high AS NOT MATERIALIZED (SELECT id, name FROM Employees WHERE salary > 100000.0) "
        "SELECT name FROM high WHERE id = 1;");
    EXPECT_EQ(std::get<Select>(notMat).ctes[0].materializeMode, MaterializeMode::NotMaterialized);
}

TEST(NestedSqlTests, MaterializedCteExplainNoteAndTempIndexLookup) {
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
        "EXPLAIN WITH high AS MATERIALIZED (SELECT id, name, salary FROM Employees WHERE salary > "
        "100000.0) SELECT name FROM high WHERE id = 1;"));
    ASSERT_TRUE(explain.success);
    const auto text = explain.rows.front().front().toString();
    EXPECT_NE(text.find("materialized CTE high"), std::string::npos);
    EXPECT_EQ(text.find("inlined CTE"), std::string::npos);
    // Outer predicate hits the ephemeral table index, not the base-table id index path note alone.
    EXPECT_NE(text.find("hash index equality lookup"), std::string::npos);

    auto result = executor.execute(parser.parse(
        "WITH high AS MATERIALIZED (SELECT id, name, salary FROM Employees WHERE salary > 100000.0) "
        "SELECT name FROM high WHERE id = 1;"));
    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.rows.size(), 1U);
    EXPECT_EQ(result.rows[0][0], Value{"Alice"});
}

TEST(NestedSqlTests, NotMaterializedCteStillInlinesToBaseIndex) {
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
                        "90000.0);"))
                    .success);
    ASSERT_TRUE(executor.execute(parser.parse("CREATE INDEX idx_id ON Employees(id);")).success);

    auto explain = executor.execute(parser.parse(
        "EXPLAIN WITH high AS NOT MATERIALIZED (SELECT id, name, salary FROM Employees WHERE "
        "salary > 100000.0) SELECT name FROM high WHERE id = 1;"));
    ASSERT_TRUE(explain.success);
    const auto text = explain.rows.front().front().toString();
    EXPECT_NE(text.find("inlined CTE high"), std::string::npos);
    EXPECT_EQ(text.find("materialized CTE"), std::string::npos);
}

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

TEST(NestedSqlTests, MultiLevelCorrelationIsRejected) {
    Parser parser;
    EXPECT_THROW(
        (void)parser.parse(
            "SELECT name FROM Employees WHERE EXISTS (SELECT emp_id FROM Bonuses WHERE EXISTS "
            "(SELECT id FROM Employees e2 WHERE e2.id = Employees.id));"),
        std::runtime_error);
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

TEST(NestedSqlTests, CteBodyMultiJoinAndAggregateProjection) {
    auto executor = makeExecutor();
    Parser parser;
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "CREATE TABLE Employees (id INT, name STRING, dept_id INT, salary DOUBLE);"))
                    .success);
    ASSERT_TRUE(
        executor.execute(parser.parse("CREATE TABLE Departments (id INT, office_id INT);")).success);
    ASSERT_TRUE(
        executor.execute(parser.parse("CREATE TABLE Offices (id INT, city STRING);")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "INSERT INTO Employees VALUES (1, \"Alice\", 10, 120000.0), "
                        "(2, \"Bob\", 10, 80000.0);"))
                    .success);
    ASSERT_TRUE(
        executor.execute(parser.parse("INSERT INTO Departments VALUES (10, 100);")).success);
    ASSERT_TRUE(executor.execute(parser.parse("INSERT INTO Offices VALUES (100, \"SF\");")).success);

    auto result = executor.execute(parser.parse(
        "WITH joined AS ("
        "  SELECT Employees.name, Offices.city, Employees.salary FROM Employees "
        "  JOIN Departments ON Employees.dept_id = Departments.id "
        "  JOIN Offices ON Departments.office_id = Offices.id"
        ") SELECT COUNT(*), SUM(salary) FROM joined;"));
    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.rows.size(), 1U);
    EXPECT_EQ(result.rows[0][0], Value{2});
    EXPECT_EQ(result.rows[0][1], Value{200000.0});
}

} // namespace VertexDB
