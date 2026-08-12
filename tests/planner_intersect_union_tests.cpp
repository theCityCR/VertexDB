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

TEST(PlannerBehaviorTests, MultiIndexIntersectChosenWhenCheaperThanSingleIndexResidual) {
    Parser parser;
    auto executor = makeExecutor("multi-index-intersect");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "CREATE TABLE Employees (id INT, dept INT, city INT, name STRING);"))
                    .success);
    ASSERT_TRUE(executor.execute(parser.parse("CREATE INDEX idx_dept ON Employees(dept);")).success);
    ASSERT_TRUE(executor.execute(parser.parse("CREATE INDEX idx_city ON Employees(city);")).success);

    for (int i = 1; i <= 100; ++i) {
        ASSERT_TRUE(executor
                        .execute(parser.parse("INSERT INTO Employees VALUES (" +
                                              std::to_string(i) + ", " + std::to_string(i % 2) +
                                              ", " + std::to_string(i % 2) + ", \"n" +
                                              std::to_string(i) + "\");"))
                        .success);
    }

    auto explain = executor.execute(
        parser.parse("EXPLAIN SELECT name FROM Employees WHERE dept = 1 AND city = 1;"));
    ASSERT_TRUE(explain.success);
    const auto text = explain.rows.front().front().toString();
    EXPECT_NE(text.find("multi-index intersect on"), std::string::npos);
    EXPECT_NE(text.find("dept"), std::string::npos);
    EXPECT_NE(text.find("city"), std::string::npos);

    auto result = executor.execute(
        parser.parse("SELECT name FROM Employees WHERE dept = 1 AND city = 1 ORDER BY id LIMIT 3;"));
    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.rows.size(), 3U);
}

TEST(PlannerBehaviorTests, MultiIndexIntersectIncludesExpressionEquality) {
    Table table{"Employees", {{"a", ColumnType::Int}, {"b", ColumnType::Int}, {"name", ColumnType::String}}};
    for (int i = 1; i <= 100; ++i) {
        table.insert({Value{i % 2}, Value{i % 2}, Value{"n" + std::to_string(i)}});
    }
    ASSERT_TRUE(table.createIndex("idx_a", "a"));
    IndexExpression bPlus;
    bPlus.kind = IndexExpression::Kind::Add;
    bPlus.column = "b";
    bPlus.literal = Value{0};
    ASSERT_TRUE(table.createIndex("idx_b_plus", bPlus));

    Predicate where =
        makeAnd(makeComparison("a", ComparisonOperator::Equal, Value{1}),
                makeExpressionComparison(bPlus, ComparisonOperator::Equal, Value{1}));
    Select query{"Employees", {}, {SelectExpr::makeStar()}, where, {}, {}};
    const auto plan = QueryPlanner{}.planSelect(query, table);
    EXPECT_EQ(plan.accessPath(), AccessPath::Intersect);
    ASSERT_EQ(std::get<IntersectPlan>(plan.path).children.size(), 2U);
    EXPECT_EQ(std::get<IntersectPlan>(plan.path).children[0].kind, IndexBitmapNode::Kind::Probe);
    EXPECT_EQ(std::get<IntersectPlan>(plan.path).children[1].kind, IndexBitmapNode::Kind::Probe);
    EXPECT_NE(plan.estimates.explanation.find("multi-index intersect on"), std::string::npos);
    EXPECT_NE(plan.estimates.explanation.find("a"), std::string::npos);
    EXPECT_NE(plan.estimates.explanation.find("b"), std::string::npos);
}

TEST(PlannerBehaviorTests, TopLevelOrIndexUnionAcrossColumns) {
    Table table{"Employees", {{"id", ColumnType::Int}, {"dept", ColumnType::Int}}};
    for (int i = 1; i <= 20; ++i) {
        table.insert({Value{i}, Value{i % 2}});
    }
    ASSERT_TRUE(table.createIndex("idx_id", "id"));
    ASSERT_TRUE(table.createIndex("idx_dept", "dept"));

    Predicate where =
        makeOr(makeComparison("id", ComparisonOperator::Equal, Value{1}),
               makeComparison("dept", ComparisonOperator::Equal, Value{0}));
    Select query{"Employees", {}, {SelectExpr::makeStar()}, where, {}, {}};
    const auto plan = QueryPlanner{}.planSelect(query, table);
    EXPECT_EQ(plan.accessPath(), AccessPath::Union);
    ASSERT_EQ(std::get<UnionPlan>(plan.path).children.size(), 2U);
    EXPECT_NE(plan.estimates.explanation.find("multi-index union on"), std::string::npos);
    EXPECT_NE(plan.estimates.explanation.find("id"), std::string::npos);
    EXPECT_NE(plan.estimates.explanation.find("dept"), std::string::npos);
    EXPECT_FALSE(plan.residual().has_value());
}

TEST(PlannerBehaviorTests, TopLevelOrIndexUnionReturnsRowsMatchingAnyProbe) {
    auto executor = makeExecutor("multi-index-union-result");
    Parser parser;
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "CREATE TABLE Employees (id INT, dept INT, city INT, name STRING);"))
                    .success);
    ASSERT_TRUE(executor.execute(parser.parse("CREATE INDEX idx_dept ON Employees(dept);")).success);
    ASSERT_TRUE(executor.execute(parser.parse("CREATE INDEX idx_city ON Employees(city);")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "INSERT INTO Employees VALUES "
                        "(1, 1, 9, \"only-dept\"), (2, 9, 1, \"only-city\"), "
                        "(3, 1, 1, \"both\"), (4, 0, 0, \"neither\");"))
                    .success);

    auto explain = executor.execute(
        parser.parse("EXPLAIN SELECT name FROM Employees WHERE dept = 1 OR city = 1;"));
    ASSERT_TRUE(explain.success);
    const auto text = explain.rows.front().front().toString();
    EXPECT_NE(text.find("multi-index union on"), std::string::npos);
    EXPECT_NE(text.find("dept"), std::string::npos);
    EXPECT_NE(text.find("city"), std::string::npos);

    auto result = executor.execute(
        parser.parse("SELECT name FROM Employees WHERE dept = 1 OR city = 1 ORDER BY name;"));
    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.rows.size(), 3U);
    EXPECT_EQ(result.rows[0][0], Value{std::string{"both"}});
    EXPECT_EQ(result.rows[1][0], Value{std::string{"only-city"}});
    EXPECT_EQ(result.rows[2][0], Value{std::string{"only-dept"}});
}

TEST(PlannerBehaviorTests, TopLevelOrWithNonIndexableDisjunctUsesPartialUnion) {
    Table table{"Employees", {{"id", ColumnType::Int}, {"name", ColumnType::String}}};
    for (int i = 1; i <= 10; ++i) {
        table.insert({Value{i}, Value{"n" + std::to_string(i)}});
    }
    ASSERT_TRUE(table.createIndex("idx_id", "id"));

    Predicate where =
        makeOr(makeComparison("id", ComparisonOperator::Equal, Value{1}),
               makeComparison("name", ComparisonOperator::Equal, Value{std::string{"n2"}}));
    Select query{"Employees", {}, {SelectExpr::makeStar()}, where, {}, {}};
    const auto plan = QueryPlanner{}.planSelect(query, table);
    EXPECT_EQ(plan.accessPath(), AccessPath::Union);
    ASSERT_EQ(std::get<UnionPlan>(plan.path).children.size(), 1U);
    EXPECT_EQ(std::get<UnionPlan>(plan.path).children.front().kind, IndexBitmapNode::Kind::Probe);
    EXPECT_EQ(std::get<UnionPlan>(plan.path).children.front().probe.column, "id");
    ASSERT_TRUE(plan.residual().has_value());
    EXPECT_NE(plan.estimates.explanation.find("multi-index union on"), std::string::npos);
    EXPECT_NE(plan.estimates.explanation.find("id"), std::string::npos);
    ASSERT_FALSE(plan.estimates.notes.empty());
    EXPECT_NE(plan.estimates.notes.front().find("residual OR"), std::string::npos);
}

TEST(PlannerBehaviorTests, TopLevelOrPartialUnionReturnsIndexableAndResidualRows) {
    auto executor = makeExecutor("partial-or-union-result");
    Parser parser;
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "CREATE TABLE Employees (id INT, name STRING, city INT);"))
                    .success);
    ASSERT_TRUE(executor.execute(parser.parse("CREATE INDEX idx_id ON Employees(id);")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "INSERT INTO Employees VALUES "
                        "(1, \"alice\", 0), (2, \"bob\", 0), (3, \"carol\", 0), (4, \"dave\", 0);"))
                    .success);

    auto explain = executor.execute(
        parser.parse("EXPLAIN SELECT name FROM Employees WHERE id = 1 OR name = \"bob\";"));
    ASSERT_TRUE(explain.success);
    const auto text = explain.rows.front().front().toString();
    EXPECT_NE(text.find("multi-index union on"), std::string::npos);
    EXPECT_NE(text.find("id"), std::string::npos);
    EXPECT_NE(text.find("residual: yes"), std::string::npos);

    auto result = executor.execute(parser.parse(
        "SELECT name FROM Employees WHERE id = 1 OR name = \"bob\" ORDER BY name;"));
    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.rows.size(), 2U);
    EXPECT_EQ(result.rows[0][0], Value{std::string{"alice"}});
    EXPECT_EQ(result.rows[1][0], Value{std::string{"bob"}});
}

TEST(PlannerBehaviorTests, TopLevelOrPartialUnionDoesNotDropIndexOnlyMatches) {
    // Residual must be complementary (OR), not an AND filter on index hits.
    auto executor = makeExecutor("partial-or-complementary");
    Parser parser;
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(
        executor.execute(parser.parse("CREATE TABLE T (id INT, flag INT, name STRING);")).success);
    ASSERT_TRUE(executor.execute(parser.parse("CREATE INDEX idx_id ON T(id);")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "INSERT INTO T VALUES (1, 0, \"indexed\"), (2, 1, \"residual\"), "
                        "(3, 0, \"neither\");"))
                    .success);

    auto result = executor.execute(
        parser.parse("SELECT name FROM T WHERE id = 1 OR flag = 1 ORDER BY name;"));
    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.rows.size(), 2U);
    EXPECT_EQ(result.rows[0][0], Value{std::string{"indexed"}});
    EXPECT_EQ(result.rows[1][0], Value{std::string{"residual"}});
}

TEST(PlannerBehaviorTests, TopLevelOrIndexUnionIncludesExpressionEquality) {
    Table table{"Employees", {{"a", ColumnType::Int}, {"b", ColumnType::Int}, {"name", ColumnType::String}}};
    for (int i = 1; i <= 100; ++i) {
        table.insert({Value{i % 10}, Value{i % 10}, Value{"n" + std::to_string(i)}});
    }
    ASSERT_TRUE(table.createIndex("idx_a", "a"));
    IndexExpression bPlus;
    bPlus.kind = IndexExpression::Kind::Add;
    bPlus.column = "b";
    bPlus.literal = Value{0};
    ASSERT_TRUE(table.createIndex("idx_b_plus", bPlus));

    Predicate where =
        makeOr(makeComparison("a", ComparisonOperator::Equal, Value{1}),
               makeExpressionComparison(bPlus, ComparisonOperator::Equal, Value{2}));
    Select query{"Employees", {}, {SelectExpr::makeStar()}, where, {}, {}};
    const auto plan = QueryPlanner{}.planSelect(query, table);
    EXPECT_EQ(plan.accessPath(), AccessPath::Union);
    ASSERT_EQ(std::get<UnionPlan>(plan.path).children.size(), 2U);
    EXPECT_NE(plan.estimates.explanation.find("multi-index union on"), std::string::npos);
}

TEST(PlannerBehaviorTests, MultiIndexIntersectReturnsOnlyRowsMatchingAllProbes) {
    auto executor = makeExecutor("multi-index-result");
    Parser parser;
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "CREATE TABLE Employees (id INT, dept INT, city INT, name STRING);"))
                    .success);
    ASSERT_TRUE(executor.execute(parser.parse("CREATE INDEX idx_dept ON Employees(dept);")).success);
    ASSERT_TRUE(executor.execute(parser.parse("CREATE INDEX idx_city ON Employees(city);")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "INSERT INTO Employees VALUES "
                        "(1, 1, 9, \"only-dept\"), (2, 9, 1, \"only-city\"), "
                        "(3, 1, 1, \"both-a\"), (4, 1, 1, \"both-b\");"))
                    .success);

    auto explain = executor.execute(
        parser.parse("EXPLAIN SELECT name FROM Employees WHERE dept = 1 AND city = 1;"));
    ASSERT_TRUE(explain.success);
    EXPECT_NE(explain.rows.front().front().toString().find("multi-index intersect"),
              std::string::npos);

    auto result = executor.execute(
        parser.parse("SELECT name FROM Employees WHERE dept = 1 AND city = 1 ORDER BY name;"));
    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.rows.size(), 2U);
    EXPECT_EQ(result.rows[0][0], Value{std::string{"both-a"}});
    EXPECT_EQ(result.rows[1][0], Value{std::string{"both-b"}});
}

TEST(PlannerBehaviorTests, ScaledMultiIndexIntersectUsesBothIndexes) {
    // Medium-cardinality dept/city so neither probe is near-unique and intersect stays cheaper
    // than a single index + residual. Seed via Table::insert for CI speed; EXPLAIN/SELECT still
    // go through the full planner/executor path. 10k matches the CTE scaled-wedge lower bound.
    constexpr std::int64_t kRowCount = 10000;

    Parser parser;
    auto executor = makeExecutor("multi-index-scale");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "CREATE TABLE Employees (id INT, dept INT, city INT, name STRING);"))
                    .success);

    auto database = executor.currentDatabase();
    ASSERT_NE(database, nullptr);
    auto table = database->table("Employees");
    ASSERT_NE(table, nullptr);

    for (std::int64_t id = 1; id <= kRowCount; ++id) {
        const auto dept = static_cast<std::int64_t>(id % 10);
        const auto city = static_cast<std::int64_t>((id / 10) % 10);
        table->insert({Value{id}, Value{dept}, Value{city}, Value{"n" + std::to_string(id)}});
    }
    ASSERT_TRUE(table->createIndex("idx_dept", "dept"));
    ASSERT_TRUE(table->createIndex("idx_city", "city"));

    auto explain = executor.execute(
        parser.parse("EXPLAIN SELECT name FROM Employees WHERE dept = 1 AND city = 1;"));
    ASSERT_TRUE(explain.success);
    ASSERT_FALSE(explain.rows.empty());
    const auto text = explain.rows.front().front().toString();
    EXPECT_NE(text.find("multi-index intersect on"), std::string::npos);
    EXPECT_NE(text.find("dept"), std::string::npos);
    EXPECT_NE(text.find("city"), std::string::npos);
    EXPECT_EQ(text.find("full table scan"), std::string::npos);

    auto result = executor.execute(parser.parse(
        "SELECT name FROM Employees WHERE dept = 1 AND city = 1 ORDER BY id;"));
    ASSERT_TRUE(result.success);
    ASSERT_FALSE(result.rows.empty());
    for (const auto &row : result.rows) {
        // Names are "n" + id; id must satisfy id%10==1 and (id/10)%10==1.
        const auto name = row[0].toString();
        ASSERT_FALSE(name.empty());
        ASSERT_EQ(name.front(), 'n');
        const auto id = std::stoll(name.substr(1));
        EXPECT_EQ(id % 10, 1);
        EXPECT_EQ((id / 10) % 10, 1);
    }
    EXPECT_EQ(result.rows.size(), static_cast<std::size_t>(kRowCount / 100));
}

TEST(PlannerBehaviorTests, NestedOrUnderAndUsesCompositeIntersectUnion) {
    // Medium-cardinality equality on the AND side so Intersect∪Union beats HashEq + residual.
    Table table{"Employees",
                {{"id", ColumnType::Int}, {"dept", ColumnType::Int}, {"city", ColumnType::Int}}};
    for (int i = 1; i <= 100; ++i) {
        table.insert({Value{i % 10}, Value{i % 10}, Value{i % 10}});
    }
    ASSERT_TRUE(table.createIndex("idx_id", "id"));
    ASSERT_TRUE(table.createIndex("idx_dept", "dept"));
    ASSERT_TRUE(table.createIndex("idx_city", "city"));

    Predicate nestedOr =
        makeOr(makeComparison("dept", ComparisonOperator::Equal, Value{1}),
               makeComparison("city", ComparisonOperator::Equal, Value{2}));
    Predicate where =
        makeAnd(makeComparison("id", ComparisonOperator::Equal, Value{1}), nestedOr);
    Select query{"Employees", {}, {SelectExpr::makeStar()}, where, {}, {}};
    const auto plan = QueryPlanner{}.planSelect(query, table);

    EXPECT_EQ(plan.accessPath(), AccessPath::Intersect);
    const auto &intersect = std::get<IntersectPlan>(plan.path);
    ASSERT_EQ(intersect.children.size(), 2U);
    EXPECT_EQ(intersect.children[0].kind, IndexBitmapNode::Kind::Probe);
    EXPECT_EQ(intersect.children[0].probe.column, "id");
    EXPECT_EQ(intersect.children[1].kind, IndexBitmapNode::Kind::Union);
    ASSERT_EQ(intersect.children[1].children.size(), 2U);
    EXPECT_FALSE(plan.residual().has_value());
    EXPECT_NE(plan.estimates.explanation.find("multi-index intersect on"), std::string::npos);
    EXPECT_NE(plan.estimates.explanation.find("union("), std::string::npos);
    ASSERT_FALSE(plan.estimates.notes.empty());
    EXPECT_NE(plan.estimates.notes.front().find("composite Intersect"), std::string::npos);
}

TEST(PlannerBehaviorTests, NestedOrUnderAndCompositeReturnsCorrectRows) {
    auto executor = makeExecutor("nested-or-intersect-union");
    Parser parser;
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "CREATE TABLE Employees (id INT, dept INT, city INT, name STRING);"))
                    .success);
    ASSERT_TRUE(executor.execute(parser.parse("CREATE INDEX idx_id ON Employees(id);")).success);
    ASSERT_TRUE(executor.execute(parser.parse("CREATE INDEX idx_dept ON Employees(dept);")).success);
    ASSERT_TRUE(executor.execute(parser.parse("CREATE INDEX idx_city ON Employees(city);")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "INSERT INTO Employees VALUES "
                        "(1, 1, 0, \"id-dept\"), (2, 0, 2, \"id-miss\"), "
                        "(1, 0, 2, \"id-city\"), (3, 1, 2, \"no-id\");"))
                    .success);

    auto explain = executor.execute(parser.parse(
        "EXPLAIN SELECT name FROM Employees WHERE id = 1 AND (dept = 1 OR city = 2);"));
    ASSERT_TRUE(explain.success);
    const auto text = explain.rows.front().front().toString();
    EXPECT_NE(text.find("multi-index intersect on"), std::string::npos);
    EXPECT_NE(text.find("union("), std::string::npos);
    EXPECT_NE(text.find("residual: no"), std::string::npos);

    auto result = executor.execute(parser.parse(
        "SELECT name FROM Employees WHERE id = 1 AND (dept = 1 OR city = 2) ORDER BY name;"));
    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.rows.size(), 2U);
    EXPECT_EQ(result.rows[0][0], Value{std::string{"id-city"}});
    EXPECT_EQ(result.rows[1][0], Value{std::string{"id-dept"}});
}

TEST(PlannerBehaviorTests, TwoOrsUnderAndUsesIntersectOfUnions) {
    Table table{"T",
                {{"a", ColumnType::Int},
                 {"b", ColumnType::Int},
                 {"c", ColumnType::Int},
                 {"d", ColumnType::Int}}};
    for (int i = 1; i <= 80; ++i) {
        table.insert({Value{i % 4}, Value{i % 4}, Value{i % 4}, Value{i % 4}});
    }
    ASSERT_TRUE(table.createIndex("idx_a", "a"));
    ASSERT_TRUE(table.createIndex("idx_b", "b"));
    ASSERT_TRUE(table.createIndex("idx_c", "c"));
    ASSERT_TRUE(table.createIndex("idx_d", "d"));

    Predicate leftOr = makeOr(makeComparison("a", ComparisonOperator::Equal, Value{1}),
                              makeComparison("b", ComparisonOperator::Equal, Value{2}));
    Predicate rightOr = makeOr(makeComparison("c", ComparisonOperator::Equal, Value{1}),
                               makeComparison("d", ComparisonOperator::Equal, Value{2}));
    Predicate where = makeAnd(leftOr, rightOr);
    Select query{"T", {}, {SelectExpr::makeStar()}, where, {}, {}};
    const auto plan = QueryPlanner{}.planSelect(query, table);

    EXPECT_EQ(plan.accessPath(), AccessPath::Intersect);
    const auto &intersect = std::get<IntersectPlan>(plan.path);
    ASSERT_EQ(intersect.children.size(), 2U);
    EXPECT_EQ(intersect.children[0].kind, IndexBitmapNode::Kind::Union);
    EXPECT_EQ(intersect.children[1].kind, IndexBitmapNode::Kind::Union);
    EXPECT_FALSE(plan.residual().has_value());
    EXPECT_NE(plan.estimates.explanation.find("union("), std::string::npos);
}

} // namespace VertexDB
