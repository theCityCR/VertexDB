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

TEST(PlannerBehaviorTests, UpdateAndDeleteWherePlansHashIndexWhenPredicateColumnIndexed) {
    Table table{"Employees",
                {{"id", ColumnType::Int}, {"name", ColumnType::String}, {"salary", ColumnType::Double}}};
    for (int i = 1; i <= 3; ++i) {
        table.insert({Value{i}, Value{"n" + std::to_string(i)}, Value{100000.0 + i}});
    }
    ASSERT_TRUE(table.createIndex("idx_id", "id"));

    Select updateScan{"Employees", {}, {SelectExpr::makeStar()},
                      makeComparison("id", ComparisonOperator::Equal, Value{1}),
                      {},
                      {}};
    const auto updatePlan = QueryPlanner{}.planSelect(updateScan, table);
    EXPECT_EQ(updatePlan.accessPath(), AccessPath::HashEq);
    EXPECT_EQ(std::get<HashEqPlan>(updatePlan.path).indexColumn, "id");
    EXPECT_FALSE(updatePlan.residual().has_value());

    Select deleteScan{"Employees", {}, {SelectExpr::makeStar()},
                      makeComparison("id", ComparisonOperator::Equal, Value{2}),
                      {},
                      {}};
    const auto deletePlan = QueryPlanner{}.planSelect(deleteScan, table);
    EXPECT_EQ(deletePlan.accessPath(), AccessPath::HashEq);
}

TEST(PlannerBehaviorTests, UpdateAndDeleteUseIndexAccessForEqualityPredicate) {
    Parser parser;
    auto executor = makeExecutor("dml-index");
    seedEmployees(executor, parser, true, false);

    ASSERT_TRUE(
        executor.execute(parser.parse("UPDATE Employees SET name = \"Alicia\" WHERE id = 1;"))
            .success);
    auto updated = executor.execute(parser.parse("SELECT name FROM Employees WHERE id = 1;"));
    ASSERT_TRUE(updated.success);
    ASSERT_EQ(updated.rows.size(), 1U);
    EXPECT_EQ(updated.rows[0][0], Value{"Alicia"});

    ASSERT_TRUE(executor.execute(parser.parse("DELETE FROM Employees WHERE id = 2;")).success);
    auto remaining =
        executor.execute(parser.parse("SELECT id FROM Employees ORDER BY id ASC;"));
    ASSERT_TRUE(remaining.success);
    ASSERT_EQ(remaining.rows.size(), 2U);
    EXPECT_EQ(remaining.rows[0][0], Value{static_cast<std::int64_t>(1)});
    EXPECT_EQ(remaining.rows[1][0], Value{static_cast<std::int64_t>(3)});
}

TEST(PlannerBehaviorTests, ExplainUpdateAndDeleteShowsHashIndexAccessPath) {
    Parser parser;
    auto executor = makeExecutor("explain-mutation");
    seedEmployees(executor, parser, true, false);

    auto explainUpdate = executor.execute(
        parser.parse("EXPLAIN UPDATE Employees SET name = \"x\" WHERE id = 1;"));
    ASSERT_TRUE(explainUpdate.success);
    ASSERT_FALSE(explainUpdate.rows.empty());
    const auto updateText = explainUpdate.rows.front().front().toString();
    EXPECT_NE(updateText.find("update:"), std::string::npos);
    EXPECT_NE(updateText.find("hash index"), std::string::npos);

    auto explainDelete =
        executor.execute(parser.parse("EXPLAIN DELETE FROM Employees WHERE id = 2;"));
    ASSERT_TRUE(explainDelete.success);
    ASSERT_FALSE(explainDelete.rows.empty());
    const auto deleteText = explainDelete.rows.front().front().toString();
    EXPECT_NE(deleteText.find("delete:"), std::string::npos);
    EXPECT_NE(deleteText.find("hash index"), std::string::npos);

    // EXPLAIN must not mutate.
    auto stillThere =
        executor.execute(parser.parse("SELECT id FROM Employees ORDER BY id ASC;"));
    ASSERT_TRUE(stillThere.success);
    EXPECT_EQ(stillThere.rows.size(), 3U);
}

TEST(PlannerBehaviorTests, UpdateUsesIndexProbeWithResidualAndFilter) {
    Parser parser;
    auto executor = makeExecutor("dml-residual");
    seedEmployees(executor, parser, true, false);

    // Indexed equality + residual name filter: only Alice matches both arms.
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "UPDATE Employees SET salary = 1.0 WHERE id = 1 AND name = \"Alice\";"))
                    .success);
    auto alice = executor.execute(parser.parse("SELECT salary FROM Employees WHERE id = 1;"));
    ASSERT_TRUE(alice.success);
    ASSERT_EQ(alice.rows.size(), 1U);
    EXPECT_EQ(alice.rows[0][0], Value{1.0});

    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "UPDATE Employees SET salary = 2.0 WHERE id = 2 AND name = \"Nope\";"))
                    .success);
    auto bob = executor.execute(parser.parse("SELECT salary FROM Employees WHERE id = 2;"));
    ASSERT_TRUE(bob.success);
    ASSERT_EQ(bob.rows.size(), 1U);
    EXPECT_EQ(bob.rows[0][0], Value{90000.0});
}

TEST(PlannerBehaviorTests, DeleteUsesOrderedRangeWhenSalaryIndexed) {
    Parser parser;
    auto executor = makeExecutor("dml-range");
    seedEmployees(executor, parser, false, true);

    Select scan{"Employees", {}, {SelectExpr::makeStar()},
                makeComparison("salary", ComparisonOperator::Greater, Value{100000.0}),
                {},
                {}};
    // Plan against a table that mirrors the seeded index so the desired path is Hash/Ordered.
    Table table{"Employees",
                {{"id", ColumnType::Int}, {"name", ColumnType::String}, {"salary", ColumnType::Double}}};
    table.insert({Value{1}, Value{"Alice"}, Value{120000.0}});
    table.insert({Value{2}, Value{"Bob"}, Value{90000.0}});
    table.insert({Value{3}, Value{"Cara"}, Value{110000.0}});
    ASSERT_TRUE(table.createIndex("idx_salary", "salary"));
    const auto plan = QueryPlanner{}.planSelect(scan, table);
    EXPECT_EQ(plan.accessPath(), AccessPath::OrderedRange);

    ASSERT_TRUE(
        executor.execute(parser.parse("DELETE FROM Employees WHERE salary > 100000.0;")).success);
    auto remaining =
        executor.execute(parser.parse("SELECT name FROM Employees ORDER BY name ASC;"));
    ASSERT_TRUE(remaining.success);
    ASSERT_EQ(remaining.rows.size(), 1U);
    EXPECT_EQ(remaining.rows[0][0], Value{"Bob"});
}

TEST(PlannerBehaviorTests, UpdateWithoutWhereStillMutatesAllVisibleRows) {
    Parser parser;
    auto executor = makeExecutor("dml-no-where");
    seedEmployees(executor, parser, true, false);

    ASSERT_TRUE(executor.execute(parser.parse("UPDATE Employees SET name = \"X\";")).success);
    auto names = executor.execute(parser.parse("SELECT name FROM Employees;"));
    ASSERT_TRUE(names.success);
    ASSERT_EQ(names.rows.size(), 3U);
    for (const auto &row : names.rows) {
        EXPECT_EQ(row[0], Value{"X"});
    }
}

TEST(PlannerBehaviorTests, UpdateAndDeleteUseHashInForInListPredicate) {
    Parser parser;
    auto executor = makeExecutor("dml-hashin");
    seedEmployees(executor, parser, true, false);

    Select scan{"Employees", {}, {SelectExpr::makeStar()},
                makeInList("id", {Value{1}, Value{3}}),
                {},
                {}};
    Table table{"Employees",
                {{"id", ColumnType::Int}, {"name", ColumnType::String}, {"salary", ColumnType::Double}}};
    table.insert({Value{1}, Value{"Alice"}, Value{120000.0}});
    table.insert({Value{2}, Value{"Bob"}, Value{90000.0}});
    table.insert({Value{3}, Value{"Cara"}, Value{110000.0}});
    ASSERT_TRUE(table.createIndex("idx_id", "id"));
    EXPECT_EQ(QueryPlanner{}.planSelect(scan, table).accessPath(), AccessPath::HashIn);

    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "UPDATE Employees SET name = \"Hit\" WHERE id IN (1, 3);"))
                    .success);
    auto names =
        executor.execute(parser.parse("SELECT name FROM Employees ORDER BY id ASC;"));
    ASSERT_TRUE(names.success);
    ASSERT_EQ(names.rows.size(), 3U);
    EXPECT_EQ(names.rows[0][0], Value{"Hit"});
    EXPECT_EQ(names.rows[1][0], Value{"Bob"});
    EXPECT_EQ(names.rows[2][0], Value{"Hit"});

    auto deleted = executor.execute(parser.parse("DELETE FROM Employees WHERE id IN (1, 3);"));
    ASSERT_TRUE(deleted.success) << deleted.message;
    EXPECT_EQ(deleted.message, "deleted 2 row(s)") << deleted.message;
    auto remaining =
        executor.execute(parser.parse("SELECT id, name FROM Employees ORDER BY id ASC;"));
    ASSERT_TRUE(remaining.success);
    ASSERT_EQ(remaining.rows.size(), 1U) << remaining.message;
    if (!remaining.rows.empty()) {
        EXPECT_EQ(remaining.rows[0][0], Value{static_cast<std::int64_t>(2)});
        EXPECT_EQ(remaining.rows[0][1], Value{"Bob"});
    }
}

TEST(PlannerBehaviorTests, DeleteUsesPrefixLikeWhenNameIndexed) {
    Parser parser;
    auto executor = makeExecutor("dml-prefix-like");
    seedEmployees(executor, parser, false, false);
    ASSERT_TRUE(executor.execute(parser.parse("CREATE INDEX idx_name ON Employees(name);")).success);

    Select scan{"Employees", {}, {SelectExpr::makeStar()}, makeLike("name", "A%"), {}, {}};
    Table table{"Employees",
                {{"id", ColumnType::Int}, {"name", ColumnType::String}, {"salary", ColumnType::Double}}};
    table.insert({Value{1}, Value{"Alice"}, Value{120000.0}});
    table.insert({Value{2}, Value{"Bob"}, Value{90000.0}});
    table.insert({Value{3}, Value{"Cara"}, Value{110000.0}});
    ASSERT_TRUE(table.createIndex("idx_name", "name"));
    EXPECT_EQ(QueryPlanner{}.planSelect(scan, table).accessPath(), AccessPath::PrefixLike);

    ASSERT_TRUE(executor.execute(parser.parse("DELETE FROM Employees WHERE name LIKE \"A%\";")).success);
    auto remaining =
        executor.execute(parser.parse("SELECT name FROM Employees ORDER BY name ASC;"));
    ASSERT_TRUE(remaining.success);
    ASSERT_EQ(remaining.rows.size(), 2U);
    EXPECT_EQ(remaining.rows[0][0], Value{"Bob"});
    EXPECT_EQ(remaining.rows[1][0], Value{"Cara"});
}

TEST(PlannerBehaviorTests, UpdateIndexedColumnViaIndexProbeCollectsTargetsFirst) {
    // Collect-then-mutate: changing the indexed probe column must not skip/double-hit rows.
    Parser parser;
    auto executor = makeExecutor("dml-collect-first");
    seedEmployees(executor, parser, true, false);

    ASSERT_TRUE(executor.execute(parser.parse("UPDATE Employees SET id = 99 WHERE id = 1;")).success);
    auto rows =
        executor.execute(parser.parse("SELECT id, name FROM Employees ORDER BY name ASC;"));
    ASSERT_TRUE(rows.success);
    ASSERT_EQ(rows.rows.size(), 3U);
    EXPECT_EQ(rows.rows[0][0], Value{static_cast<std::int64_t>(99)});
    EXPECT_EQ(rows.rows[0][1], Value{"Alice"});
    EXPECT_EQ(rows.rows[1][0], Value{static_cast<std::int64_t>(2)});
    EXPECT_EQ(rows.rows[2][0], Value{static_cast<std::int64_t>(3)});
}

TEST(PlannerBehaviorTests, UpdateUsesMultiIndexIntersectPath) {
    Parser parser;
    auto executor = makeExecutor("dml-intersect");
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
    EXPECT_NE(explain.rows.front().front().toString().find("multi-index intersect on"),
              std::string::npos);

    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "UPDATE Employees SET name = \"hit\" WHERE dept = 1 AND city = 1;"))
                    .success);
    auto hits =
        executor.execute(parser.parse("SELECT id FROM Employees WHERE name = \"hit\";"));
    ASSERT_TRUE(hits.success);
    EXPECT_EQ(hits.rows.size(), 50U);

    auto missed =
        executor.execute(parser.parse("SELECT name FROM Employees WHERE dept = 0 LIMIT 1;"));
    ASSERT_TRUE(missed.success);
    ASSERT_EQ(missed.rows.size(), 1U);
    EXPECT_NE(missed.rows[0][0], Value{"hit"});
}

} // namespace VertexDB
