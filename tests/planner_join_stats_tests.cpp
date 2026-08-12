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

TEST(PlannerBehaviorTests, StatsDrivenPlannerPrefersSelectiveEqualityOverLowCardinality) {
    Table table{"Employees",
                {{"id", ColumnType::Int}, {"dept", ColumnType::Int}, {"salary", ColumnType::Double}}};
    for (int i = 1; i <= 100; ++i) {
        table.insert({Value{i}, Value{i % 2}, Value{100000.0 + i}});
    }
    ASSERT_TRUE(table.createIndex("idx_id", "id"));
    ASSERT_TRUE(table.createIndex("idx_dept", "dept"));

    EXPECT_EQ(table.indexDistinctCount("id"), std::optional<std::size_t>{100});
    EXPECT_EQ(table.indexDistinctCount("dept"), std::optional<std::size_t>{2});

    Predicate where =
        makeAnd(makeComparison("dept", ComparisonOperator::Equal, Value{1}),
                makeComparison("id", ComparisonOperator::Equal, Value{50}));
    Select query{"Employees", {}, {SelectExpr::makeStar()}, where, {}, {}};
    const auto plan = QueryPlanner{}.planSelect(query, table);
    EXPECT_EQ(plan.accessPath(), AccessPath::HashEq);
    EXPECT_EQ(std::get<HashEqPlan>(plan.path).indexColumn, "id");
    EXPECT_LT(plan.estimates.estimatedCost, 2.0);
    ASSERT_TRUE(plan.residual().has_value());
    EXPECT_EQ(std::get<ComparisonPred>(*plan.residual()).column, "dept");
}

TEST(PlannerBehaviorTests, StatsDrivenInLookupCostsScaleWithDistinctKeys) {
    Table table{"Employees", {{"id", ColumnType::Int}, {"dept", ColumnType::Int}}};
    for (int i = 1; i <= 90; ++i) {
        table.insert({Value{i}, Value{i % 3}});
    }
    ASSERT_TRUE(table.createIndex("idx_id", "id"));
    ASSERT_TRUE(table.createIndex("idx_dept", "dept"));

    Predicate where =
        makeAnd(makeInList("dept", {Value{0}, Value{1}, Value{2}}),
                makeComparison("id", ComparisonOperator::Equal, Value{7}));
    Select query{"Employees", {}, {SelectExpr::makeStar()}, where, {}, {}};
    const auto plan = QueryPlanner{}.planSelect(query, table);
    EXPECT_EQ(plan.accessPath(), AccessPath::HashEq);
    EXPECT_EQ(std::get<HashEqPlan>(plan.path).indexColumn, "id");
    EXPECT_LT(plan.estimates.estimatedCost, 3.0);
}

TEST(PlannerBehaviorTests, StatsDrivenJoinChoosesNestedLoopIndexProbe) {
    Parser parser;
    auto executor = makeExecutor("stats-join-nl");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(
        executor
            .execute(parser.parse("CREATE TABLE Employees (id INT, name STRING, dept_id INT);"))
            .success);
    ASSERT_TRUE(
        executor.execute(parser.parse("CREATE TABLE Departments (id INT, dept STRING);")).success);
    ASSERT_TRUE(executor.execute(parser.parse("CREATE INDEX idx_dept_id ON Departments(id);"))
                    .success);

    ASSERT_TRUE(executor.execute(parser.parse("INSERT INTO Employees VALUES (1, \"Alice\", 10);"))
                    .success);
    ASSERT_TRUE(executor.execute(parser.parse("INSERT INTO Employees VALUES (2, \"Bob\", 20);"))
                    .success);
    for (int i = 1; i <= 200; ++i) {
        ASSERT_TRUE(executor
                        .execute(parser.parse("INSERT INTO Departments VALUES (" +
                                              std::to_string(i) + ", \"D" + std::to_string(i) +
                                              "\");"))
                        .success);
    }

    auto explain = executor.execute(
        parser.parse("EXPLAIN SELECT Employees.name, Departments.dept FROM Employees JOIN "
                     "Departments ON dept_id = id;"));
    ASSERT_TRUE(explain.success);
    const auto text = explain.rows.front().front().toString();
    EXPECT_NE(text.find("nested loop join (index probe on Departments.id)"), std::string::npos);

    auto result = executor.execute(
        parser.parse("SELECT Employees.name, Departments.dept FROM Employees JOIN Departments ON "
                     "dept_id = id ORDER BY Employees.id;"));
    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.rows.size(), 2U);
    EXPECT_EQ(result.rows[0][0], Value{"Alice"});
    EXPECT_EQ(result.rows[0][1], Value{"D10"});
    EXPECT_EQ(result.rows[1][0], Value{"Bob"});
    EXPECT_EQ(result.rows[1][1], Value{"D20"});
}

TEST(PlannerBehaviorTests, StatsDrivenJoinKeepsHashWhenNeitherSideIndexed) {
    Table left{"Employees", {{"id", ColumnType::Int}, {"dept_id", ColumnType::Int}}};
    Table right{"Departments", {{"id", ColumnType::Int}, {"dept", ColumnType::String}}};
    left.insert({Value{1}, Value{10}});
    right.insert({Value{10}, Value{"Eng"}});

    JoinClause join{"Departments", "dept_id", "id"};
    const auto plan = QueryPlanner{}.planJoin(left, right, join);
    EXPECT_EQ(plan.algorithm, JoinAlgorithm::HashJoin);
    EXPECT_NE(plan.explanation.find("hash join"), std::string::npos);
}

TEST(PlannerBehaviorTests, OuterAndCrossJoinsPreferNestedLoop) {
    Table left{"Employees", {{"id", ColumnType::Int}, {"dept_id", ColumnType::Int}}};
    Table right{"Departments", {{"id", ColumnType::Int}, {"dept", ColumnType::String}}};
    left.insert({Value{1}, Value{10}});
    right.insert({Value{10}, Value{"Eng"}});
    right.createIndex("idx_id", "id");

    JoinClause rightJoin{"Departments", "dept_id", "id", std::nullopt, JoinKind::RightOuter};
    auto rightPlan = QueryPlanner{}.planJoin(left, right, rightJoin);
    EXPECT_EQ(rightPlan.algorithm, JoinAlgorithm::NestedLoopIndexProbe);
    EXPECT_NE(rightPlan.explanation.find("right outer"), std::string::npos);

    JoinClause fullJoin{"Departments", "dept_id", "id", std::nullopt, JoinKind::FullOuter};
    auto fullPlan = QueryPlanner{}.planJoin(left, right, fullJoin);
    EXPECT_EQ(fullPlan.algorithm, JoinAlgorithm::NestedLoopIndexProbe);
    EXPECT_NE(fullPlan.explanation.find("full outer"), std::string::npos);

    JoinClause crossJoin{"Departments", "", "", std::nullopt, JoinKind::Cross};
    auto crossPlan = QueryPlanner{}.planJoin(left, right, crossJoin);
    EXPECT_EQ(crossPlan.algorithm, JoinAlgorithm::NestedLoopIndexProbe);
    EXPECT_NE(crossPlan.explanation.find("cross"), std::string::npos);
}

} // namespace VertexDB
