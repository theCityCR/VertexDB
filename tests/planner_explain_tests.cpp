#include "planner_test_support.hpp"
#include "test_support.hpp"

#include "VertexDB/execution/query_executor.hpp"
#include "VertexDB/execution/sql_literal.hpp"
#include "VertexDB/indexing/btree_index.hpp"
#include "VertexDB/parser/parser.hpp"
#include "VertexDB/persistence/physical_redo.hpp"
#include "VertexDB/persistence/storage_manager.hpp"
#include "VertexDB/persistence/write_ahead_log.hpp"
#include "VertexDB/planner/query_planner.hpp"
#include "VertexDB/planner/rewriter.hpp"
#include "VertexDB/storage/database.hpp"
#include "VertexDB/storage/row_store.hpp"
#include "VertexDB/storage/table.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace VertexDB {
namespace {

using planner_test::makeExecutor;
using planner_test::StubIndexCatalog;
using planner_test::StubRelationStats;

} // namespace

TEST(PlannerBehaviorTests, ExplainReportsJoinPlanFromCostModel) {
    Parser parser;
    auto executor = makeExecutor("explain-join");
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

    auto explain = executor.execute(
        parser.parse("EXPLAIN SELECT * FROM Employees JOIN Departments ON dept_id = id;"));
    ASSERT_TRUE(explain.success);
    ASSERT_FALSE(explain.rows.empty());
    const auto text = explain.rows.front().front().toString();
    EXPECT_NE(text.find("hash join"), std::string::npos);
    EXPECT_EQ(text.find("planner bypassed"), std::string::npos);
    EXPECT_NE(text.find("cost="), std::string::npos);
}

TEST(PlannerBehaviorTests, ExplainAnalyzeReportsActualRowsMatchingCardinality) {
    Parser parser;
    auto executor = makeExecutor("explain-analyze-eq");
    seedEmployees(executor, parser, true, false);

    auto plain =
        executor.execute(parser.parse("EXPLAIN SELECT name FROM Employees WHERE id = 1;"));
    ASSERT_TRUE(plain.success);
    const auto plainText = plain.rows.front().front().toString();
    EXPECT_NE(plainText.find("est_rows="), std::string::npos);
    EXPECT_EQ(plainText.find("actual_rows="), std::string::npos);
    EXPECT_EQ(plainText.find("actual_time_ms="), std::string::npos);

    auto analyzed =
        executor.execute(parser.parse("EXPLAIN ANALYZE SELECT name FROM Employees WHERE id = 1;"));
    ASSERT_TRUE(analyzed.success);
    const auto text = analyzed.rows.front().front().toString();
    EXPECT_NE(text.find("est_rows="), std::string::npos);
    EXPECT_NE(text.find("actual_rows=1"), std::string::npos);
    EXPECT_NE(text.find("actual_time_ms="), std::string::npos);
    EXPECT_EQ(text.find("candidates="), std::string::npos);

    auto select =
        executor.execute(parser.parse("SELECT name FROM Employees WHERE id = 1;"));
    ASSERT_TRUE(select.success);
    EXPECT_EQ(select.rows.size(), 1U);
}

TEST(PlannerBehaviorTests, ExplainAnalyzeReportsCandidatesWhenResidualDropsRows) {
    Parser parser;
    auto executor = makeExecutor("explain-analyze-residual");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "CREATE TABLE Employees (id INT, dept INT, name STRING);"))
                    .success);
    ASSERT_TRUE(executor.execute(parser.parse("CREATE INDEX idx_dept ON Employees(dept);")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "INSERT INTO Employees VALUES "
                        "(1, 1, \"a\"), (2, 1, \"b\"), (3, 1, \"c\"), (4, 2, \"d\");"))
                    .success);

    auto analyzed = executor.execute(
        parser.parse("EXPLAIN ANALYZE SELECT name FROM Employees WHERE dept = 1 AND id = 1;"));
    ASSERT_TRUE(analyzed.success);
    const auto text = analyzed.rows.front().front().toString();
    EXPECT_NE(text.find("residual: yes"), std::string::npos);
    EXPECT_NE(text.find("actual_rows=1"), std::string::npos);
    EXPECT_NE(text.find("candidates=3"), std::string::npos);
    EXPECT_NE(text.find("actual_time_ms="), std::string::npos);
}

TEST(PlannerBehaviorTests, ExplainAnalyzeJoinReportsActualRowsPerStep) {
    Parser parser;
    auto executor = makeExecutor("explain-analyze-join");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse("CREATE TABLE Employees (id INT, dept_id INT, name STRING);"))
                    .success);
    ASSERT_TRUE(
        executor.execute(parser.parse("CREATE TABLE Departments (id INT, name STRING);")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "INSERT INTO Employees VALUES (1, 10, \"Alice\"), (2, 10, \"Bob\"), "
                        "(3, 20, \"Cara\");"))
                    .success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "INSERT INTO Departments VALUES (10, \"Eng\"), (20, \"Sales\");"))
                    .success);

    auto analyzed = executor.execute(parser.parse(
        "EXPLAIN ANALYZE SELECT Employees.name FROM Employees "
        "JOIN Departments ON Employees.dept_id = Departments.id;"));
    ASSERT_TRUE(analyzed.success);
    ASSERT_FALSE(analyzed.rows.empty());
    const auto text = analyzed.rows.front().front().toString();
    EXPECT_NE(text.find("est_rows="), std::string::npos);
    EXPECT_NE(text.find("actual_rows=3"), std::string::npos);
    EXPECT_NE(text.find("actual_time_ms="), std::string::npos);
}

TEST(PlannerBehaviorTests, ExplainAnalyzeReportsActualRowsAfterAggregation) {
    // Desired (docs/sql.md): with aggregates/GROUP BY, actual_rows is the post-aggregation
    // cardinality, and the aggregation marker still appears.
    Parser parser;
    auto executor = makeExecutor("explain-analyze-agg");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "CREATE TABLE Employees (id INT, dept_id INT, name STRING);"))
                    .success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "INSERT INTO Employees VALUES (1, 10, \"Alice\"), (2, 10, \"Bob\"), "
                        "(3, 20, \"Cara\");"))
                    .success);

    auto analyzed = executor.execute(parser.parse(
        "EXPLAIN ANALYZE SELECT dept_id, COUNT(*) FROM Employees GROUP BY dept_id;"));
    ASSERT_TRUE(analyzed.success);
    ASSERT_FALSE(analyzed.rows.empty());
    const auto text = analyzed.rows.front().front().toString();
    EXPECT_NE(text.find("actual_rows=2"), std::string::npos);
    EXPECT_NE(text.find("actual_time_ms="), std::string::npos);
    bool sawAggregation = false;
    for (const auto &row : analyzed.rows) {
        if (row.front().toString() == "aggregation") {
            sawAggregation = true;
        }
    }
    EXPECT_TRUE(sawAggregation);

    auto countStar =
        executor.execute(parser.parse("EXPLAIN ANALYZE SELECT COUNT(*) FROM Employees;"));
    ASSERT_TRUE(countStar.success);
    EXPECT_NE(countStar.rows.front().front().toString().find("actual_rows=1"), std::string::npos);
}

TEST(PlannerBehaviorTests, ExplainAnalyzeActualRowsIgnoreLimit) {
    // Desired (docs/sql.md): actual_rows is pre-ORDER BY / LIMIT cardinality.
    Parser parser;
    auto executor = makeExecutor("explain-analyze-limit");
    seedEmployees(executor, parser, false, false);

    auto analyzed = executor.execute(
        parser.parse("EXPLAIN ANALYZE SELECT name FROM Employees ORDER BY name LIMIT 1;"));
    ASSERT_TRUE(analyzed.success);
    const auto text = analyzed.rows.front().front().toString();
    EXPECT_NE(text.find("actual_rows=3"), std::string::npos)
        << "LIMIT must not shrink EXPLAIN ANALYZE actual_rows; got:\n"
        << text;

    auto limited = executor.execute(parser.parse("SELECT name FROM Employees ORDER BY name LIMIT 1;"));
    ASSERT_TRUE(limited.success);
    EXPECT_EQ(limited.rows.size(), 1U);
}

TEST(PlannerBehaviorTests, ExplainAnalyzeWithReportsActuals) {
    // Desired: EXPLAIN ANALYZE WITH … uses the same single-pass execute path as SELECT.
    // Inlined CTE keeps the body salary filter as a residual over the outer id probe.
    Parser parser;
    auto executor = makeExecutor("explain-analyze-with");
    seedEmployees(executor, parser, true, false);

    auto analyzed = executor.execute(parser.parse(
        "EXPLAIN ANALYZE WITH high AS ("
        "SELECT id, name, salary FROM Employees WHERE salary > 100000.0) "
        "SELECT name FROM high WHERE id = 1;"));
    ASSERT_TRUE(analyzed.success);
    ASSERT_FALSE(analyzed.rows.empty());
    const auto text = analyzed.rows.front().front().toString();
    EXPECT_NE(text.find("actual_rows=1"), std::string::npos);
    EXPECT_NE(text.find("candidates=1"), std::string::npos);
    EXPECT_NE(text.find("actual_time_ms="), std::string::npos);
    bool sawInlineNote = false;
    for (const auto &row : analyzed.rows) {
        if (row.front().toString().find("inlined CTE") != std::string::npos) {
            sawInlineNote = true;
        }
    }
    EXPECT_TRUE(sawInlineNote);
}

} // namespace VertexDB
