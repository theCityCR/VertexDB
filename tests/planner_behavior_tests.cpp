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

QueryExecutor makeExecutor(std::string_view suffix) {
    const auto root =
        std::filesystem::temp_directory_path() / ("vertexdb-planner-" + std::string(suffix));
    std::filesystem::remove_all(root);
    return QueryExecutor{root};
}

void seedEmployees(QueryExecutor &executor, Parser &parser, bool indexId = true,
                   bool indexSalary = false) {
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
    if (indexId) {
        ASSERT_TRUE(executor.execute(parser.parse("CREATE INDEX idx_id ON Employees(id);")).success);
    }
    if (indexSalary) {
        ASSERT_TRUE(
            executor.execute(parser.parse("CREATE INDEX idx_salary ON Employees(salary);")).success);
    }
}

} // namespace

TEST(PlannerBehaviorTests, ResidualFilterRejectsIndexedHitsThatFailRemainingConjuncts) {
    Parser parser;
    auto executor = makeExecutor("residual-reject");
    seedEmployees(executor, parser, true, false);

    // id=2 hits the hash index, but salary residual must reject Bob (90000).
    auto result = executor.execute(
        parser.parse("SELECT name FROM Employees WHERE id = 2 AND salary > 100000.0;"));
    ASSERT_TRUE(result.success);
    EXPECT_TRUE(result.rows.empty());

    auto explain = executor.execute(
        parser.parse("EXPLAIN SELECT name FROM Employees WHERE id = 2 AND salary > 100000.0;"));
    ASSERT_TRUE(explain.success);
    const auto text = explain.rows.front().front().toString();
    EXPECT_NE(text.find("hash index equality lookup"), std::string::npos);
    EXPECT_NE(text.find("residual: yes"), std::string::npos);
}

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

TEST(PlannerBehaviorTests, PlannerAndExplainUseOrderedIndexForLessThan) {
    Table table{"Employees", {{"id", ColumnType::Int}, {"salary", ColumnType::Double}}};
    table.insert({Value{1}, Value{120000.0}});
    table.insert({Value{2}, Value{90000.0}});
    ASSERT_TRUE(table.createIndex("idx_salary", "salary"));

    QueryPlanner planner;
    Select less{"Employees", {},
                {SelectExpr::makeStar()},       Predicate{"salary", ComparisonOperator::Less, Value{100000.0}},
                {},          {}};
    const auto plan = planner.planSelect(less, table);
    EXPECT_EQ(plan.accessPath, AccessPath::OrderedIndexRange);
    EXPECT_EQ(plan.indexOp, ComparisonOperator::Less);
    EXPECT_FALSE(plan.residual.has_value());

    Parser parser;
    auto executor = makeExecutor("less-than");
    seedEmployees(executor, parser, false, true);
    auto explain = executor.execute(
        parser.parse("EXPLAIN SELECT name FROM Employees WHERE salary < 100000.0;"));
    ASSERT_TRUE(explain.success);
    EXPECT_NE(explain.rows.front().front().toString().find("ordered index range lookup"),
              std::string::npos);

    auto result =
        executor.execute(parser.parse("SELECT name FROM Employees WHERE salary < 100000.0;"));
    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.rows.size(), 1U);
    EXPECT_EQ(result.rows[0][0], Value{"Bob"});
}

TEST(PlannerBehaviorTests, OrPredicateFullScanIsDocumentedLimitation) {
    // Intentional v1 limitation (docs/sql.md): OR is not split for indexing.
    Table table{"Employees", {{"id", ColumnType::Int}}};
    table.insert({Value{1}});
    table.insert({Value{2}});
    ASSERT_TRUE(table.createIndex("idx_id", "id"));

    Predicate orPredicate{
        Predicate::Kind::Or,
        std::make_shared<Predicate>(Predicate{"id", ComparisonOperator::Equal, Value{1}}),
        std::make_shared<Predicate>(Predicate{"id", ComparisonOperator::Equal, Value{2}})};
    Select query{"Employees", {}, {SelectExpr::makeStar()}, orPredicate, {}, {}};
    QueryPlanner planner;
    const auto plan = planner.planSelect(query, table);
    EXPECT_EQ(plan.accessPath, AccessPath::FullScan);
    EXPECT_NE(plan.explanation.find("OR predicate"), std::string::npos);

    Parser parser;
    auto executor = makeExecutor("or-explain");
    seedEmployees(executor, parser, true, false);
    auto explain = executor.execute(
        parser.parse("EXPLAIN SELECT name FROM Employees WHERE id = 1 OR id = 2;"));
    ASSERT_TRUE(explain.success);
    EXPECT_NE(explain.rows.front().front().toString().find("full table scan (OR predicate)"),
              std::string::npos);
}

TEST(PlannerBehaviorTests, InSubqueryFallsBackToScanWhenOuterColumnUnindexed) {
    Parser parser;
    auto executor = makeExecutor("in-unindexed");
    seedEmployees(executor, parser, false, true);

    auto explain = executor.execute(parser.parse(
        "EXPLAIN SELECT name FROM Employees WHERE id IN (SELECT id FROM Employees WHERE salary > "
        "100000.0);"));
    ASSERT_TRUE(explain.success);
    EXPECT_NE(explain.rows.front().front().toString().find("full table scan"), std::string::npos);

    auto result = executor.execute(parser.parse(
        "SELECT name FROM Employees WHERE id IN (SELECT id FROM Employees WHERE salary > "
        "100000.0) ORDER BY name ASC;"));
    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.rows.size(), 2U);
    EXPECT_EQ(result.rows[0][0], Value{"Alice"});
    EXPECT_EQ(result.rows[1][0], Value{"Cara"});
}

TEST(PlannerBehaviorTests, IndexPreferredOverFullScanWhenCostsAreTied) {
    Table table{"Employees", {{"id", ColumnType::Int}}};
    table.insert({Value{1}});
    ASSERT_TRUE(table.createIndex("idx_id", "id"));

    Select query{"Employees", {},
                 {SelectExpr::makeStar()},       Predicate{"id", ComparisonOperator::Equal, Value{1}},
                 {},          {}};
    const auto plan = QueryPlanner{}.planSelect(query, table);
    EXPECT_EQ(plan.accessPath, AccessPath::HashIndexLookup);
}

TEST(PlannerBehaviorTests, MultiConjunctAndPicksCheapestIndexableAndKeepsResidualTree) {
    Table table{"Employees",
                {{"id", ColumnType::Int}, {"dept", ColumnType::Int}, {"salary", ColumnType::Double}}};
    table.insert({Value{1}, Value{10}, Value{120000.0}});
    ASSERT_TRUE(table.createIndex("idx_id", "id"));
    ASSERT_TRUE(table.createIndex("idx_salary", "salary"));

    Predicate leafId{"id", ComparisonOperator::Equal, Value{1}};
    Predicate leafDept{"dept", ComparisonOperator::Equal, Value{10}};
    Predicate leafSalary{"salary", ComparisonOperator::Greater, Value{100000.0}};
    Predicate mid{Predicate::Kind::And, std::make_shared<Predicate>(leafId),
                  std::make_shared<Predicate>(leafDept)};
    Predicate where{Predicate::Kind::And, std::make_shared<Predicate>(mid),
                    std::make_shared<Predicate>(leafSalary)};

    Select query{"Employees", {}, {SelectExpr::makeStar()}, where, {}, {}};
    const auto plan = QueryPlanner{}.planSelect(query, table);
    EXPECT_EQ(plan.accessPath, AccessPath::HashIndexLookup);
    EXPECT_EQ(plan.indexColumn, "id");
    ASSERT_TRUE(plan.residual.has_value());
    EXPECT_EQ(plan.residual->kind, Predicate::Kind::And);
}

TEST(PlannerBehaviorTests, ExplainReportsNoResidualForPureEqualityIndexLookup) {
    Parser parser;
    auto executor = makeExecutor("residual-no");
    seedEmployees(executor, parser, true, false);

    auto explain =
        executor.execute(parser.parse("EXPLAIN SELECT name FROM Employees WHERE id = 1;"));
    ASSERT_TRUE(explain.success);
    const auto text = explain.rows.front().front().toString();
    EXPECT_NE(text.find("hash index equality lookup"), std::string::npos);
    EXPECT_NE(text.find("residual: no"), std::string::npos);
}

TEST(PlannerBehaviorTests, UpdateAndDeleteUseFullScanEvenWhenPredicateColumnIndexed) {
    // Intentional v1 limitation (docs/sql.md): UPDATE/DELETE do not use planner index paths.
    Parser parser;
    auto executor = makeExecutor("dml-scan");
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

    Predicate where{
        Predicate::Kind::And,
        std::make_shared<Predicate>(Predicate{"dept", ComparisonOperator::Equal, Value{1}}),
        std::make_shared<Predicate>(Predicate{"id", ComparisonOperator::Equal, Value{50}})};
    Select query{"Employees", {}, {SelectExpr::makeStar()}, where, {}, {}};
    const auto plan = QueryPlanner{}.planSelect(query, table);
    EXPECT_EQ(plan.accessPath, AccessPath::HashIndexLookup);
    EXPECT_EQ(plan.indexColumn, "id");
    EXPECT_LT(plan.estimatedCost, 2.0);
    ASSERT_TRUE(plan.residual.has_value());
    EXPECT_EQ(plan.residual->column, "dept");
}

TEST(PlannerBehaviorTests, StatsDrivenInLookupCostsScaleWithDistinctKeys) {
    Table table{"Employees", {{"id", ColumnType::Int}, {"dept", ColumnType::Int}}};
    for (int i = 1; i <= 90; ++i) {
        table.insert({Value{i}, Value{i % 3}});
    }
    ASSERT_TRUE(table.createIndex("idx_id", "id"));
    ASSERT_TRUE(table.createIndex("idx_dept", "dept"));

    Predicate where{
        Predicate::Kind::And,
        std::make_shared<Predicate>(
            Predicate{"dept", std::vector<Value>{Value{0}, Value{1}, Value{2}}}),
        std::make_shared<Predicate>(Predicate{"id", ComparisonOperator::Equal, Value{7}})};
    Select query{"Employees", {}, {SelectExpr::makeStar()}, where, {}, {}};
    const auto plan = QueryPlanner{}.planSelect(query, table);
    EXPECT_EQ(plan.accessPath, AccessPath::HashIndexLookup);
    EXPECT_EQ(plan.indexColumn, "id");
    EXPECT_LT(plan.estimatedCost, 3.0);
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

    Predicate where{
        Predicate::Kind::And,
        std::make_shared<Predicate>(Predicate{"a", ComparisonOperator::Equal, Value{1}}),
        std::make_shared<Predicate>(
            Predicate::makeExpressionComparison(bPlus, ComparisonOperator::Equal, Value{1}))};
    Select query{"Employees", {}, {SelectExpr::makeStar()}, where, {}, {}};
    const auto plan = QueryPlanner{}.planSelect(query, table);
    EXPECT_EQ(plan.accessPath, AccessPath::MultiIndexIntersect);
    ASSERT_EQ(plan.intersectProbes.size(), 2U);
    EXPECT_NE(plan.explanation.find("multi-index intersect on"), std::string::npos);
    EXPECT_NE(plan.explanation.find("a"), std::string::npos);
    EXPECT_NE(plan.explanation.find("b"), std::string::npos);
}

TEST(PlannerBehaviorTests, TopLevelOrIndexUnionRemainsDocumentedLimitation) {
    Table table{"Employees", {{"id", ColumnType::Int}, {"dept", ColumnType::Int}}};
    for (int i = 1; i <= 20; ++i) {
        table.insert({Value{i}, Value{i % 2}});
    }
    ASSERT_TRUE(table.createIndex("idx_id", "id"));
    ASSERT_TRUE(table.createIndex("idx_dept", "dept"));

    Predicate where{
        Predicate::Kind::Or,
        std::make_shared<Predicate>(Predicate{"id", ComparisonOperator::Equal, Value{1}}),
        std::make_shared<Predicate>(Predicate{"dept", ComparisonOperator::Equal, Value{0}})};
    Select query{"Employees", {}, {SelectExpr::makeStar()}, where, {}, {}};
    const auto plan = QueryPlanner{}.planSelect(query, table);
    EXPECT_EQ(plan.accessPath, AccessPath::FullScan);
    EXPECT_NE(plan.explanation.find("OR"), std::string::npos);
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

TEST(PlannerBehaviorTests, HistogramAwareRangeCostBeatsDefaultOneThirdEstimate) {
    Table table{"Employees", {{"id", ColumnType::Int}, {"score", ColumnType::Int}}};
    for (int i = 1; i <= 90; ++i) {
        table.insert({Value{i}, Value{i}});
    }
    ASSERT_TRUE(table.createIndex("idx_score", "score"));

    Predicate where{"score", ComparisonOperator::Greater, Value{80}};
    Select query{"Employees", {}, {SelectExpr::makeStar()}, where, {}, {}};

    const auto before = QueryPlanner{}.planSelect(query, table);
    EXPECT_EQ(before.accessPath, AccessPath::OrderedIndexRange);
    EXPECT_NEAR(before.estimatedCost, 30.0, 0.5); // N/3 fallback

    table.analyze();
    const auto after = QueryPlanner{}.planSelect(query, table);
    EXPECT_EQ(after.accessPath, AccessPath::OrderedIndexRange);
    EXPECT_LT(after.estimatedCost, before.estimatedCost);
    EXPECT_LE(after.estimatedCost, 15.0);
}

TEST(PlannerBehaviorTests, ExpressionIndexSubtractEqualityLookup) {
    auto executor = makeExecutor("expr-subtract");
    Parser parser;
    seedEmployees(executor, parser, false, false);
    ASSERT_TRUE(
        executor.execute(parser.parse("CREATE INDEX idx_id_minus ON Employees((id-1));")).success);

    auto explain =
        executor.execute(parser.parse("EXPLAIN SELECT name FROM Employees WHERE (id-1) = 0;"));
    ASSERT_TRUE(explain.success);
    EXPECT_NE(explain.rows.front().front().toString().find("expression hash index"),
              std::string::npos);

    auto result = executor.execute(parser.parse("SELECT name FROM Employees WHERE (id-1) = 0;"));
    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.rows.size(), 1U);
    EXPECT_EQ(result.rows[0][0], Value{std::string{"Alice"}});
}

TEST(PlannerBehaviorTests, DoubleExpressionIndexAndHistogramLessRange) {
    auto executor = makeExecutor("double-expr-hist");
    Parser parser;
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(
        executor.execute(parser.parse("CREATE TABLE Prices (id INT, amount DOUBLE);")).success);
    for (int i = 1; i <= 40; ++i) {
        ASSERT_TRUE(executor
                        .execute(parser.parse("INSERT INTO Prices VALUES (" + std::to_string(i) +
                                              ", " + std::to_string(i) + ".5);"))
                        .success);
    }
    ASSERT_TRUE(
        executor.execute(parser.parse("CREATE INDEX idx_neg_amt ON Prices((-amount));")).success);
    ASSERT_TRUE(
        executor.execute(parser.parse("CREATE INDEX idx_amt ON Prices(amount);")).success);
    ASSERT_TRUE(executor.execute(parser.parse("ANALYZE TABLE Prices;")).success);

    auto expr = executor.execute(
        parser.parse("EXPLAIN SELECT id FROM Prices WHERE (-amount) = -10.5;"));
    ASSERT_TRUE(expr.success);
    EXPECT_NE(expr.rows.front().front().toString().find("expression"), std::string::npos);

    auto lessExplain =
        executor.execute(parser.parse("EXPLAIN SELECT id FROM Prices WHERE amount < 5.5;"));
    ASSERT_TRUE(lessExplain.success);
    EXPECT_NE(lessExplain.rows.front().front().toString().find("ordered index range"),
              std::string::npos);

    Table table{"Scores", {{"id", ColumnType::Int}, {"score", ColumnType::Double}}};
    for (int i = 1; i <= 64; ++i) {
        table.insert({Value{i}, Value{static_cast<double>(i)}});
    }
    ASSERT_TRUE(table.createIndex("idx_score", "score"));
    table.analyze();
    Predicate where{"score", ComparisonOperator::Less, Value{10.0}};
    Select query{"Scores", {}, {SelectExpr::makeStar()}, where, {}, {}};
    const auto plan = QueryPlanner{}.planSelect(query, table);
    EXPECT_EQ(plan.accessPath, AccessPath::OrderedIndexRange);
    EXPECT_LT(plan.estimatedCost, 64.0 / 3.0);
}

} // namespace VertexDB
