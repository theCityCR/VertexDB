#include "test_support.hpp"

#include "VertexDB/execution/query_executor.hpp"
#include "VertexDB/parser/parser.hpp"
#include "VertexDB/planner/query_planner.hpp"
#include "VertexDB/planner/rewriter.hpp"

#include <gtest/gtest.h>

#include <filesystem>
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

TEST(NestedSqlTests, NestedWithDeeperThanOneLevelIsRejected) {
    Parser parser;
    EXPECT_THROW((void)parser.parse(
                     "WITH a AS (WITH b AS (WITH c AS (SELECT id FROM Employees) SELECT id FROM c) "
                     "SELECT id FROM b) SELECT id FROM a;"),
                 std::runtime_error);
}

TEST(NestedSqlTests, NestedSqlDocumentedRefusalsAreRejected) {
    Parser parser;

    // Derived tables require an alias.
    EXPECT_THROW((void)parser.parse("SELECT id FROM (SELECT id FROM Employees);"),
                 std::runtime_error);

    // Outer JOIN against a CTE/derived alias is rejected (body WHERE would mis-scope on the join).
    auto cteJoin = parser.parse(
        "WITH high AS (SELECT id, name FROM Employees WHERE id = 1) "
        "SELECT * FROM high JOIN Departments ON id = id;");
    ASSERT_TRUE(std::holds_alternative<Select>(cteJoin));
    EXPECT_THROW((void)rewriteSelect(std::get<Select>(cteJoin)), std::runtime_error);

    auto derivedJoin = parser.parse(
        "SELECT * FROM (SELECT id, name FROM Employees WHERE id = 1) AS high "
        "JOIN Departments ON id = id;");
    ASSERT_TRUE(std::holds_alternative<Select>(derivedJoin));
    EXPECT_THROW((void)rewriteSelect(std::get<Select>(derivedJoin)), std::runtime_error);
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

TEST(NestedSqlTests, NestedWithAndJoinInSubqueryDocumentedRefusals) {
    Parser parser;
    // JOIN inside IN/EXISTS remains unsupported.
    EXPECT_THROW((void)parser.parse(
                     "SELECT name FROM Employees WHERE id IN (SELECT Employees.id FROM Employees "
                     "JOIN Departments ON Employees.dept_id = Departments.id);"),
                 std::runtime_error);
    EXPECT_THROW((void)parser.parse(
                     "SELECT name FROM Employees WHERE EXISTS (SELECT Employees.id FROM Employees "
                     "JOIN Departments ON Employees.dept_id = Departments.id);"),
                 std::runtime_error);
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
