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
    EXPECT_EQ(predicateKind(*withSelect.where), PredicateKind::Comparison);

    auto inQuery =
        parser.parse("SELECT name FROM Employees WHERE id IN (SELECT id FROM Employees WHERE "
                     "salary > 100000.0);");
    ASSERT_TRUE(std::holds_alternative<Select>(inQuery));
    const auto &inSelect = std::get<Select>(inQuery);
    ASSERT_TRUE(inSelect.where.has_value());
    EXPECT_EQ(predicateKind(*inSelect.where), PredicateKind::InSubquery);
    const auto &inSubquery = std::get<InSubqueryPred>(*inSelect.where);
    ASSERT_TRUE(inSubquery.subquery);
    EXPECT_EQ(inSubquery.subquery->table, "Employees");

    auto explain = parser.parse("EXPLAIN SELECT name FROM Employees WHERE id = 1;");
    ASSERT_TRUE(std::holds_alternative<ExplainQuery>(explain));
    ASSERT_TRUE(std::holds_alternative<Select>(std::get<ExplainQuery>(explain).query));
    EXPECT_EQ(std::get<Select>(std::get<ExplainQuery>(explain).query).table, "Employees");
    EXPECT_FALSE(std::get<ExplainQuery>(explain).analyze);
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
    EXPECT_EQ(predicateKind(*rewritten.query.where), PredicateKind::And);
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
    EXPECT_EQ(predicateKind(*rewritten.query.where), PredicateKind::And);
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

TEST(NestedSqlTests, InlinedCteOuterExpressionPredicateUsesExpressionIndex) {
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

    auto explain = executor.execute(parser.parse(
        "EXPLAIN WITH high AS (SELECT id, name, salary FROM Employees WHERE id > 0) "
        "SELECT name FROM high WHERE (-salary) = -120000.0;"));
    ASSERT_TRUE(explain.success);
    const auto text = explain.rows.front().front().toString();
    EXPECT_NE(text.find("inlined CTE high"), std::string::npos);
    EXPECT_NE(text.find("expression hash index"), std::string::npos);

    auto result = executor.execute(parser.parse(
        "WITH high AS (SELECT id, name, salary FROM Employees WHERE id > 0) "
        "SELECT name FROM high WHERE (-salary) = -120000.0;"));
    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.rows.size(), 1U);
    EXPECT_EQ(result.rows[0][0], Value{"Alice"});
}

TEST(NestedSqlTests, NestedWithInlinesOneLevel) {
    Parser parser;
    auto executor = makeExecutor("nested-with");
    seedEmployees(executor, parser, true, false);

    auto explain = executor.execute(parser.parse(
        "EXPLAIN WITH outer_cte AS ("
        "  WITH inner_cte AS (SELECT id, name, salary FROM Employees WHERE salary > 100000.0) "
        "  SELECT id, name FROM inner_cte WHERE id = 1"
        ") SELECT name FROM outer_cte;"));
    ASSERT_TRUE(explain.success);
    const auto text = explain.rows.front().front().toString();
    EXPECT_NE(text.find("inlined CTE"), std::string::npos);

    auto result = executor.execute(parser.parse(
        "WITH outer_cte AS ("
        "  WITH inner_cte AS (SELECT id, name, salary FROM Employees WHERE salary > 100000.0) "
        "  SELECT id, name FROM inner_cte WHERE id = 1"
        ") SELECT name FROM outer_cte;"));
    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.rows.size(), 1U);
    EXPECT_EQ(result.rows[0][0], Value{"Alice"});
}

TEST(NestedSqlTests, NestedWithInlinesThreeLevels) {
    Parser parser;
    auto executor = makeExecutor("nested-with-three");
    seedEmployees(executor, parser, true, false);

    auto result = executor.execute(parser.parse(
        "WITH a AS ("
        "  WITH b AS ("
        "    WITH c AS (SELECT id, name, salary FROM Employees WHERE salary > 100000.0) "
        "    SELECT id, name FROM c WHERE id = 1"
        "  ) SELECT id, name FROM b"
        ") SELECT name FROM a;"));
    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.rows.size(), 1U);
    EXPECT_EQ(result.rows[0][0], Value{"Alice"});
}

TEST(NestedSqlTests, NestedWithDeeperThanMaxIsRejected) {
    Parser parser;
    // depth 0..3 allowed; four nested WITH bodies inside the outermost exceeds kMaxNestedWithDepth.
    EXPECT_THROW((void)parser.parse(
                     "WITH a AS (WITH b AS (WITH c AS (WITH d AS (WITH e AS "
                     "(SELECT id FROM Employees) SELECT id FROM e) SELECT id FROM d) "
                     "SELECT id FROM c) SELECT id FROM b) SELECT id FROM a;"),
                 std::runtime_error);
}

TEST(NestedSqlTests, OuterJoinAgainstCteAndDerivedAlias) {
    Parser parser;
    auto executor = makeExecutor("outer-join-cte");
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

    auto cteJoin = parser.parse(
        "WITH high AS (SELECT id, name, dept_id FROM Employees WHERE id = 1) "
        "SELECT high.name, Departments.dept FROM high JOIN Departments "
        "ON high.dept_id = Departments.id;");
    ASSERT_TRUE(std::holds_alternative<Select>(cteJoin));
    const auto cteRewrite = rewriteSelect(std::get<Select>(cteJoin));
    ASSERT_FALSE(cteRewrite.materialize.empty());
    EXPECT_NE(cteRewrite.notes.front().find("join target"), std::string::npos);

    auto cteResult = executor.execute(cteJoin);
    ASSERT_TRUE(cteResult.success) << cteResult.message;
    ASSERT_EQ(cteResult.rows.size(), 1U);
    EXPECT_EQ(cteResult.rows[0][0], Value{"Alice"});
    EXPECT_EQ(cteResult.rows[0][1], Value{"Eng"});

    auto derivedResult = executor.execute(parser.parse(
        "SELECT high.name, Departments.dept FROM "
        "(SELECT id, name, dept_id FROM Employees WHERE id = 1) AS high "
        "JOIN Departments ON high.dept_id = Departments.id;"));
    ASSERT_TRUE(derivedResult.success) << derivedResult.message;
    ASSERT_EQ(derivedResult.rows.size(), 1U);
    EXPECT_EQ(derivedResult.rows[0][0], Value{"Alice"});
    EXPECT_EQ(derivedResult.rows[0][1], Value{"Eng"});

    auto rightCte = executor.execute(parser.parse(
        "WITH depts AS (SELECT id, dept FROM Departments) "
        "SELECT Employees.name, depts.dept FROM Employees RIGHT JOIN depts "
        "ON Employees.dept_id = depts.id ORDER BY depts.dept;"));
    ASSERT_TRUE(rightCte.success) << rightCte.message;
    ASSERT_EQ(rightCte.rows.size(), 1U);
    EXPECT_EQ(rightCte.rows[0][0], Value{"Alice"});
    EXPECT_EQ(rightCte.rows[0][1], Value{"Eng"});
}

TEST(NestedSqlTests, MaterializedCteFencesBaseTableIndex) {
    Parser parser;
    auto executor = makeExecutor("materialized-cte");
    seedEmployees(executor, parser, true, false);

    auto explain = executor.execute(parser.parse(
        "EXPLAIN WITH high AS MATERIALIZED (SELECT id, name, salary FROM Employees WHERE salary > "
        "100000.0) SELECT name FROM high WHERE id = 1;"));
    ASSERT_TRUE(explain.success);
    const auto text = explain.rows.front().front().toString();
    EXPECT_NE(text.find("materialized CTE high"), std::string::npos);
    EXPECT_EQ(text.find("inlined CTE"), std::string::npos);

    auto result = executor.execute(parser.parse(
        "WITH high AS MATERIALIZED (SELECT id, name, salary FROM Employees WHERE salary > 100000.0) "
        "SELECT name FROM high WHERE id = 1;"));
    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.rows.size(), 1U);
    EXPECT_EQ(result.rows[0][0], Value{"Alice"});
}

TEST(NestedSqlTests, DerivedTableInlinesLikeCteAndUsesBaseIndex) {
    Parser parser;
    auto executor = makeExecutor("derived-table");
    seedEmployees(executor, parser, true, false);

    auto rewritten = rewriteSelect(std::get<Select>(parser.parse(
        "SELECT name FROM (SELECT id, name, salary FROM Employees WHERE salary > 100000.0) AS high "
        "WHERE id = 1;")));
    EXPECT_EQ(rewritten.query.table, "Employees");
    ASSERT_TRUE(rewritten.query.where.has_value());
    EXPECT_EQ(predicateKind(*rewritten.query.where), PredicateKind::And);
    ASSERT_FALSE(rewritten.notes.empty());
    EXPECT_NE(rewritten.notes.front().find("inlined CTE"), std::string::npos);

    auto explain = executor.execute(parser.parse(
        "EXPLAIN SELECT name FROM (SELECT id, name, salary FROM Employees WHERE salary > 100000.0) "
        "AS high WHERE id = 1;"));
    ASSERT_TRUE(explain.success);
    const auto text = explain.rows.front().front().toString();
    EXPECT_NE(text.find("hash index equality lookup"), std::string::npos);
    EXPECT_NE(text.find("inlined CTE"), std::string::npos);

    auto result = executor.execute(parser.parse(
        "SELECT name FROM (SELECT id, name, salary FROM Employees WHERE salary > 100000.0) high "
        "WHERE id = 1;"));
    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.rows.size(), 1U);
    EXPECT_EQ(result.rows[0][0], Value{"Alice"});
}

TEST(NestedSqlTests, CteWithJoinInlinesToJoinSelect) {
    Parser parser;
    auto executor = makeExecutor("cte-join");
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

    auto explain = executor.execute(parser.parse(
        "EXPLAIN WITH joined AS (SELECT * FROM Employees JOIN Departments ON dept_id = id) "
        "SELECT Employees.name, Departments.dept FROM joined;"));
    ASSERT_TRUE(explain.success);
    ASSERT_GE(explain.rows.size(), 1U);
    EXPECT_NE(explain.rows.front().front().toString().find("hash join"), std::string::npos);
    bool sawInline = false;
    for (const auto &row : explain.rows) {
        if (row.front().toString().find("inlined CTE") != std::string::npos) {
            sawInline = true;
        }
    }
    EXPECT_TRUE(sawInline);

    auto result = executor.execute(parser.parse(
        "WITH joined AS (SELECT * FROM Employees JOIN Departments ON dept_id = id) "
        "SELECT Employees.name, Departments.dept FROM joined;"));
    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.rows.size(), 1U);
    EXPECT_EQ(result.rows[0][0], Value{std::string{"Alice"}});
    EXPECT_EQ(result.rows[0][1], Value{std::string{"Eng"}});
}

TEST(NestedSqlTests, MultiCteInliningAndUnusedCteNote) {
    Parser parser;

    auto chained = parser.parse(
        "WITH base AS (SELECT id, name, salary FROM Employees WHERE salary > 100000.0), "
        "high AS (SELECT id, name, salary FROM base WHERE id = 1) "
        "SELECT name FROM high;");
    const auto rewritten = rewriteSelect(std::get<Select>(chained));
    EXPECT_EQ(rewritten.query.table, "Employees");
    ASSERT_GE(rewritten.notes.size(), 1U);
    EXPECT_NE(rewritten.notes.front().find("inlined CTE"), std::string::npos);

    auto unused = parser.parse(
        "WITH unused AS (SELECT id FROM Employees WHERE id = 1) "
        "SELECT name FROM Employees WHERE id = 2;");
    const auto unusedRewrite = rewriteSelect(std::get<Select>(unused));
    EXPECT_EQ(unusedRewrite.query.table, "Employees");
    ASSERT_FALSE(unusedRewrite.notes.empty());
    EXPECT_NE(unusedRewrite.notes.front().find("FROM references a base table"), std::string::npos);
}

TEST(NestedSqlTests, SiblingCtesInlineRecursively) {
    auto executor = makeExecutor("sibling-ctes");
    Parser parser;
    seedEmployees(executor, parser, true, false);
    auto explain = executor.execute(parser.parse(
        "EXPLAIN WITH high AS (SELECT id, name, salary FROM Employees WHERE salary > 100000.0), "
        "picked AS (SELECT name FROM high WHERE id = 1) SELECT name FROM picked;"));
    ASSERT_TRUE(explain.success);
    const auto text = explain.rows.front().front().toString();
    EXPECT_NE(text.find("inlined CTE"), std::string::npos);
    auto result = executor.execute(parser.parse(
        "WITH high AS (SELECT id, name, salary FROM Employees WHERE salary > 100000.0), "
        "picked AS (SELECT name FROM high WHERE id = 1) SELECT name FROM picked;"));
    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.rows.size(), 1U);
    EXPECT_EQ(result.rows[0][0], Value{std::string{"Alice"}});
}

TEST(NestedSqlTests, CteInliningLeavesBodyFilterAsResidualWhenOuterUsesIndex) {
    Parser parser;
    auto executor = makeExecutor("cte-residual");
    seedEmployees(executor, parser, true, false);

    auto explain = executor.execute(parser.parse(
        "EXPLAIN WITH high AS (SELECT id, name, salary FROM Employees WHERE salary > 100000.0) "
        "SELECT name FROM high WHERE id = 1;"));
    ASSERT_TRUE(explain.success);
    const auto text = explain.rows.front().front().toString();
    EXPECT_NE(text.find("hash index equality lookup"), std::string::npos);
    EXPECT_NE(text.find("inlined CTE"), std::string::npos);
    EXPECT_NE(text.find("residual: yes"), std::string::npos);
}

TEST(NestedSqlTests, ScaledCteWinQueryUsesHashIndexAndResidual) {
    // Large enough that a materializing CTE of high-salary rows would be wasteful.
    // Seed via Table::insert (bypass per-row SQL/WAL) so the suite stays CI-friendly; EXPLAIN and
    // SELECT still go through the full rewrite/planner/executor path. 10k sits at the plan's lower
    // bound; 100k is reserved for the CTE microbenchmark once packaging continues.
    constexpr std::int64_t kRowCount = 10000;

    Parser parser;
    auto executor = makeExecutor("cte-scale");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(
        executor
            .execute(parser.parse("CREATE TABLE Employees (id INT, name STRING, salary DOUBLE);"))
            .success);

    auto database = executor.currentDatabase();
    ASSERT_NE(database, nullptr);
    auto table = database->table("Employees");
    ASSERT_NE(table, nullptr);

    table->insert({Value{static_cast<std::int64_t>(1)}, Value{"Alice"}, Value{120000.0}});
    for (std::int64_t id = 2; id <= kRowCount; ++id) {
        // Most rows match the CTE body filter; a materializing engine would build ~95k temps.
        const double salary = (id % 20 == 0) ? 80000.0 : 110000.0;
        table->insert({Value{id}, Value{"Emp"}, Value{salary}});
    }
    ASSERT_TRUE(table->createIndex("idx_id", "id"));

    auto explain = executor.execute(parser.parse(
        "EXPLAIN WITH high AS (SELECT id, name, salary FROM Employees WHERE salary > 100000.0) "
        "SELECT name FROM high WHERE id = 1;"));
    ASSERT_TRUE(explain.success);
    ASSERT_FALSE(explain.rows.empty());
    const auto text = explain.rows.front().front().toString();
    EXPECT_NE(text.find("hash index equality lookup"), std::string::npos);
    EXPECT_NE(text.find("inlined CTE"), std::string::npos);
    EXPECT_NE(text.find("residual: yes"), std::string::npos);
    EXPECT_EQ(text.find("full table scan"), std::string::npos);

    auto result = executor.execute(parser.parse(
        "WITH high AS (SELECT id, name, salary FROM Employees WHERE salary > 100000.0) "
        "SELECT name FROM high WHERE id = 1;"));
    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.rows.size(), 1U);
    EXPECT_EQ(result.rows[0][0], Value{"Alice"});
}

} // namespace VertexDB
