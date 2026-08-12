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

TEST(PlannerBehaviorTests, SelectPlanningUsesAbstractStatisticsAndIndexCatalog) {
    Select query{"Employees", {},
                 {SelectExpr::makeStar()},
                 makeComparison("id", ComparisonOperator::Equal, Value{42}),
                 {},
                 {}};
    const StubRelationStats stats;
    const StubIndexCatalog indexes;

    const auto plan = QueryPlanner{}.planSelect(query, stats, indexes);

    EXPECT_EQ(plan.accessPath(), AccessPath::HashEq);
    EXPECT_EQ(plan.estimates.estimatedRows, 1U);
}

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

TEST(PlannerBehaviorTests, PlannerAndExplainUseOrderedIndexForLessThan) {
    Table table{"Employees", {{"id", ColumnType::Int}, {"salary", ColumnType::Double}}};
    table.insert({Value{1}, Value{120000.0}});
    table.insert({Value{2}, Value{90000.0}});
    ASSERT_TRUE(table.createIndex("idx_salary", "salary"));

    QueryPlanner planner;
    Select less{"Employees", {},
                {SelectExpr::makeStar()},
                makeComparison("salary", ComparisonOperator::Less, Value{100000.0}),
                {},          {}};
    const auto plan = planner.planSelect(less, table);
    EXPECT_EQ(plan.accessPath(), AccessPath::OrderedRange);
    EXPECT_EQ(std::get<OrderedRangePlan>(plan.path).indexOp, ComparisonOperator::Less);
    EXPECT_FALSE(plan.residual().has_value());

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

TEST(PlannerBehaviorTests, TopLevelOrSameColumnEqualityUsesHashIn) {
    Table table{"Employees", {{"id", ColumnType::Int}}};
    table.insert({Value{1}});
    table.insert({Value{2}});
    ASSERT_TRUE(table.createIndex("idx_id", "id"));

    Predicate orPredicate =
        makeOr(makeComparison("id", ComparisonOperator::Equal, Value{1}),
               makeComparison("id", ComparisonOperator::Equal, Value{2}));
    Select query{"Employees", {}, {SelectExpr::makeStar()}, orPredicate, {}, {}};
    QueryPlanner planner;
    const auto plan = planner.planSelect(query, table);
    EXPECT_EQ(plan.accessPath(), AccessPath::HashIn);
    ASSERT_EQ(std::get<HashInPlan>(plan.path).indexValues.size(), 2U);
    EXPECT_EQ(std::get<HashInPlan>(plan.path).indexColumn, "id");
    EXPECT_NE(plan.estimates.explanation.find("hash index IN lookup on"), std::string::npos);
    EXPECT_FALSE(plan.residual().has_value());

    Parser parser;
    auto executor = makeExecutor("or-explain");
    seedEmployees(executor, parser, true, false);
    auto explain = executor.execute(
        parser.parse("EXPLAIN SELECT name FROM Employees WHERE id = 1 OR id = 2;"));
    ASSERT_TRUE(explain.success);
    EXPECT_NE(explain.rows.front().front().toString().find("hash index IN lookup on"),
              std::string::npos);
    EXPECT_NE(explain.rows.front().front().toString().find("residual: no"), std::string::npos);
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
                 {SelectExpr::makeStar()},
                 makeComparison("id", ComparisonOperator::Equal, Value{1}),
                 {},          {}};
    const auto plan = QueryPlanner{}.planSelect(query, table);
    EXPECT_EQ(plan.accessPath(), AccessPath::HashEq);
}

TEST(PlannerBehaviorTests, MultiConjunctAndPicksCheapestIndexableAndKeepsResidualTree) {
    Table table{"Employees",
                {{"id", ColumnType::Int}, {"dept", ColumnType::Int}, {"salary", ColumnType::Double}}};
    table.insert({Value{1}, Value{10}, Value{120000.0}});
    ASSERT_TRUE(table.createIndex("idx_id", "id"));
    ASSERT_TRUE(table.createIndex("idx_salary", "salary"));

    Predicate leafId = makeComparison("id", ComparisonOperator::Equal, Value{1});
    Predicate leafDept = makeComparison("dept", ComparisonOperator::Equal, Value{10});
    Predicate leafSalary =
        makeComparison("salary", ComparisonOperator::Greater, Value{100000.0});
    Predicate mid = makeAnd(leafId, leafDept);
    Predicate where = makeAnd(mid, leafSalary);

    Select query{"Employees", {}, {SelectExpr::makeStar()}, where, {}, {}};
    const auto plan = QueryPlanner{}.planSelect(query, table);
    EXPECT_EQ(plan.accessPath(), AccessPath::HashEq);
    EXPECT_EQ(std::get<HashEqPlan>(plan.path).indexColumn, "id");
    ASSERT_TRUE(plan.residual().has_value());
    EXPECT_EQ(predicateKind(*plan.residual()), PredicateKind::And);
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

TEST(PlannerBehaviorTests, ExpressionSameColumnOrRewritesToHashIn) {
    Table table{"Employees", {{"id", ColumnType::Int}, {"salary", ColumnType::Double}}};
    for (int i = 1; i <= 10; ++i) {
        table.insert({Value{i}, Value{100000.0 + i}});
    }
    IndexExpression negSalary;
    negSalary.kind = IndexExpression::Kind::Negate;
    negSalary.column = "salary";
    ASSERT_TRUE(table.createIndex("idx_neg_salary", negSalary));

    Predicate where =
        makeOr(makeExpressionComparison(negSalary, ComparisonOperator::Equal, Value{-100001.0}),
               makeExpressionComparison(negSalary, ComparisonOperator::Equal, Value{-100002.0}));
    Select query{"Employees", {}, {SelectExpr::makeStar()}, where, {}, {}};
    const auto plan = QueryPlanner{}.planSelect(query, table);
    EXPECT_EQ(plan.accessPath(), AccessPath::HashIn);
    ASSERT_TRUE(std::get<HashInPlan>(plan.path).indexExpression.has_value());
    EXPECT_EQ(*std::get<HashInPlan>(plan.path).indexExpression, negSalary);
    ASSERT_EQ(std::get<HashInPlan>(plan.path).indexValues.size(), 2U);
    EXPECT_FALSE(plan.residual().has_value());
}

TEST(PlannerBehaviorTests, HistogramAwareRangeCostBeatsDefaultOneThirdEstimate) {
    Table table{"Employees", {{"id", ColumnType::Int}, {"score", ColumnType::Int}}};
    for (int i = 1; i <= 90; ++i) {
        table.insert({Value{i}, Value{i}});
    }
    ASSERT_TRUE(table.createIndex("idx_score", "score"));

    Predicate where = makeComparison("score", ComparisonOperator::Greater, Value{80});
    Select query{"Employees", {}, {SelectExpr::makeStar()}, where, {}, {}};

    const auto before = QueryPlanner{}.planSelect(query, table);
    EXPECT_EQ(before.accessPath(), AccessPath::OrderedRange);
    EXPECT_NEAR(before.estimates.estimatedCost, 30.0, 0.5); // N/3 fallback

    table.analyze();
    const auto after = QueryPlanner{}.planSelect(query, table);
    EXPECT_EQ(after.accessPath(), AccessPath::OrderedRange);
    EXPECT_LT(after.estimates.estimatedCost, before.estimates.estimatedCost);
    EXPECT_LE(after.estimates.estimatedCost, 15.0);
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
    Predicate where = makeComparison("score", ComparisonOperator::Less, Value{10.0});
    Select query{"Scores", {}, {SelectExpr::makeStar()}, where, {}, {}};
    const auto plan = QueryPlanner{}.planSelect(query, table);
    EXPECT_EQ(plan.accessPath(), AccessPath::OrderedRange);
    EXPECT_LT(plan.estimates.estimatedCost, 64.0 / 3.0);
}

TEST(PlannerBehaviorTests, NestedOrUnderAndRemainsResidualWhenDisjunctsDiffer) {
    // Documented: heterogeneous OR nested under AND stays residual while another conjunct uses
    // an index. Same-column equality OR is rewritten to IN (see SameColumnNestedOrUnderAnd…).
    Table table{"Employees",
                {{"id", ColumnType::Int}, {"name", ColumnType::String}, {"dept", ColumnType::Int}}};
    table.insert({Value{1}, Value{std::string{"Alice"}}, Value{10}});
    table.insert({Value{2}, Value{std::string{"Bob"}}, Value{20}});
    ASSERT_TRUE(table.createIndex("idx_id", "id"));

    Predicate nestedOr =
        makeOr(makeComparison("name", ComparisonOperator::Equal, Value{std::string{"Alice"}}),
               makeComparison("dept", ComparisonOperator::Equal, Value{20}));
    Predicate where =
        makeAnd(makeComparison("id", ComparisonOperator::Equal, Value{1}), nestedOr);
    Select query{"Employees", {}, {SelectExpr::makeStar()}, where, {}, {}};
    const auto plan = QueryPlanner{}.planSelect(query, table);
    EXPECT_EQ(plan.accessPath(), AccessPath::HashEq);
    EXPECT_EQ(std::get<HashEqPlan>(plan.path).indexColumn, "id");
    ASSERT_TRUE(plan.residual().has_value());
    EXPECT_EQ(predicateKind(*plan.residual()), PredicateKind::Or);

    Parser parser;
    auto executor = makeExecutor("nested-or-residual");
    seedEmployees(executor, parser, true, false);
    auto explain = executor.execute(parser.parse(
        "EXPLAIN SELECT name FROM Employees WHERE id = 1 AND (name = \"Alice\" OR salary > "
        "100000.0);"));
    ASSERT_TRUE(explain.success);
    const auto text_plan = explain.rows.front().front().toString();
    EXPECT_NE(text_plan.find("hash index equality lookup"), std::string::npos);
    EXPECT_NE(text_plan.find("residual: yes"), std::string::npos);
}

TEST(PlannerBehaviorTests, SameColumnNestedOrUnderAndRewritesToHashIn) {
    Table table{"Employees", {{"id", ColumnType::Int}, {"dept", ColumnType::Int}}};
    for (int i = 1; i <= 20; ++i) {
        table.insert({Value{i}, Value{i % 3}});
    }
    ASSERT_TRUE(table.createIndex("idx_dept", "dept"));

    Predicate nestedOr =
        makeOr(makeComparison("dept", ComparisonOperator::Equal, Value{0}),
               makeComparison("dept", ComparisonOperator::Equal, Value{1}));
    Predicate where =
        makeAnd(makeComparison("id", ComparisonOperator::Greater, Value{0}), nestedOr);
    Select query{"Employees", {}, {SelectExpr::makeStar()}, where, {}, {}};
    const auto plan = QueryPlanner{}.planSelect(query, table);
    EXPECT_EQ(plan.accessPath(), AccessPath::HashIn);
    EXPECT_EQ(std::get<HashInPlan>(plan.path).indexColumn, "dept");
    ASSERT_EQ(std::get<HashInPlan>(plan.path).indexValues.size(), 2U);
    ASSERT_TRUE(plan.residual().has_value());
    EXPECT_EQ(std::get<ComparisonPred>(*plan.residual()).column, "id");

    Parser parser;
    auto executor = makeExecutor("nested-or-hashin");
    seedEmployees(executor, parser, false, false);
    ASSERT_TRUE(executor.execute(parser.parse("CREATE INDEX idx_id ON Employees(id);")).success);
    auto explain = executor.execute(
        parser.parse("EXPLAIN SELECT name FROM Employees WHERE id = 1 OR id = 2 OR id = 3;"));
    ASSERT_TRUE(explain.success);
    EXPECT_NE(explain.rows.front().front().toString().find("hash index IN lookup on"),
              std::string::npos);

    auto rows = executor.execute(
        parser.parse("SELECT name FROM Employees WHERE id = 1 OR id = 3 ORDER BY name ASC;"));
    ASSERT_TRUE(rows.success);
    ASSERT_EQ(rows.rows.size(), 2U);
    EXPECT_EQ(rows.rows[0][0], Value{"Alice"});
    EXPECT_EQ(rows.rows[1][0], Value{"Cara"});
}

TEST(PlannerBehaviorTests, SameColumnNestedOrUnderAndWithSelectiveEquality) {
    Table table{"Employees",
                {{"id", ColumnType::Int}, {"dept", ColumnType::Int}, {"name", ColumnType::String}}};
    for (int i = 1; i <= 100; ++i) {
        table.insert({Value{i}, Value{i % 2}, Value{"n" + std::to_string(i)}});
    }
    ASSERT_TRUE(table.createIndex("idx_id", "id"));
    ASSERT_TRUE(table.createIndex("idx_dept", "dept"));

    Predicate nestedOr =
        makeOr(makeComparison("dept", ComparisonOperator::Equal, Value{0}),
               makeComparison("dept", ComparisonOperator::Equal, Value{1}));
    Predicate where =
        makeAnd(makeComparison("id", ComparisonOperator::Equal, Value{50}), nestedOr);
    Select query{"Employees", {}, {SelectExpr::makeStar()}, where, {}, {}};
    const auto plan = QueryPlanner{}.planSelect(query, table);
    EXPECT_EQ(plan.accessPath(), AccessPath::HashEq);
    EXPECT_EQ(std::get<HashEqPlan>(plan.path).indexColumn, "id");
    ASSERT_TRUE(plan.residual().has_value());
    EXPECT_EQ(predicateKind(*plan.residual()), PredicateKind::InList);
}

TEST(PlannerBehaviorTests, RegexPredicateUsesResidualFullScan) {
    // Documented: col ~ pattern is always a residual full-scan filter (sql.md).
    Table table{"Employees", {{"id", ColumnType::Int}, {"name", ColumnType::String}}};
    table.insert({Value{1}, Value{std::string{"Alice"}}});
    table.insert({Value{2}, Value{std::string{"Bob"}}});
    ASSERT_TRUE(table.createIndex("idx_name", "name"));

    Predicate where = RegexPred{"name", "^A.*"};
    Select query{"Employees", {}, {SelectExpr::makeStar()}, where, {}, {}};
    const auto plan = QueryPlanner{}.planSelect(query, table);
    EXPECT_EQ(plan.accessPath(), AccessPath::FullScan);
    ASSERT_TRUE(plan.residual().has_value());
    EXPECT_EQ(predicateKind(*plan.residual()), PredicateKind::Regex);
    EXPECT_EQ(plan.estimates.explanation, "full table scan");

    Parser parser;
    auto executor = makeExecutor("regex-residual-scan");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(
        executor.execute(parser.parse("CREATE TABLE Employees (id INT, name STRING);")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "INSERT INTO Employees VALUES (1, \"Alice\"), (2, \"Bob\");"))
                    .success);
    ASSERT_TRUE(executor.execute(parser.parse("CREATE INDEX idx_name ON Employees(name);")).success);

    auto explain =
        executor.execute(parser.parse("EXPLAIN SELECT name FROM Employees WHERE name ~ \"^A\";"));
    ASSERT_TRUE(explain.success);
    const auto text = explain.rows.front().front().toString();
    EXPECT_NE(text.find("full table scan"), std::string::npos);
    EXPECT_NE(text.find("residual: yes"), std::string::npos);
}

TEST(PlannerBehaviorTests, PrefixLikeUsesOrderedIndexAccessPath) {
    Parser parser;
    auto executor = makeExecutor("prefix-like-plan");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(
        executor.execute(parser.parse("CREATE TABLE Employees (id INT, name STRING);")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "INSERT INTO Employees VALUES (1, \"Alice\"), (2, \"Bob\"), "
                        "(3, \"Alicia\");"))
                    .success);
    ASSERT_TRUE(executor.execute(parser.parse("CREATE INDEX idx_name ON Employees(name);")).success);

    auto explain = executor.execute(
        parser.parse("EXPLAIN SELECT name FROM Employees WHERE name LIKE \"Al%\";"));
    ASSERT_TRUE(explain.success);
    const auto text = explain.rows.front().front().toString();
    EXPECT_NE(text.find("ordered index prefix LIKE on name"), std::string::npos);
    EXPECT_NE(text.find("residual: yes"), std::string::npos);

    Table table{"Employees", {{"id", ColumnType::Int}, {"name", ColumnType::String}}};
    table.insert({Value{1}, Value{std::string{"Alice"}}});
    ASSERT_TRUE(table.createIndex("idx_name", "name"));
    Predicate where = LikePred{"name", "Al%"};
    Select query{"Employees", {}, {SelectExpr::makeStar()}, where, {}, {}};
    const auto plan = QueryPlanner{}.planSelect(query, table);
    EXPECT_EQ(plan.accessPath(), AccessPath::PrefixLike);
    ASSERT_TRUE(plan.residual().has_value());
    EXPECT_EQ(predicateKind(*plan.residual()), PredicateKind::Like);
}

TEST(PlannerBehaviorTests, TrigramSubstringLikeUsesIntersectAccessPath) {
    Parser parser;
    auto executor = makeExecutor("trigram-like-plan");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse("CREATE TABLE Employees (id INT, name STRING, note STRING);"))
                    .success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "INSERT INTO Employees VALUES (1, \"Alice\", \"hello world\"), "
                        "(2, \"Bob\", \"goodbye\"), (3, \"Cara\", \"say hello\");"))
                    .success);
    ASSERT_TRUE(
        executor.execute(parser.parse("CREATE INDEX idx_note_tri ON Employees((trigram(note)));"))
            .success);

    auto explain = executor.execute(
        parser.parse("EXPLAIN SELECT name FROM Employees WHERE note LIKE \"%hello%\";"));
    ASSERT_TRUE(explain.success);
    const auto text = explain.rows.front().front().toString();
    EXPECT_NE(text.find("trigram intersect for LIKE on note"), std::string::npos);
    EXPECT_NE(text.find("residual: yes"), std::string::npos);

    Table table{"Employees",
                {{"id", ColumnType::Int}, {"name", ColumnType::String}, {"note", ColumnType::String}}};
    table.insert({Value{1}, Value{std::string{"Alice"}}, Value{std::string{"hello"}}});
    IndexExpression trigram{IndexExpression::Kind::Trigram, "note", {}};
    ASSERT_TRUE(table.createIndex("idx_note_tri", trigram));
    Predicate where = LikePred{"note", "%hello%"};
    Select query{"Employees", {}, {SelectExpr::makeStar()}, where, {}, {}};
    const auto plan = QueryPlanner{}.planSelect(query, table);
    EXPECT_EQ(plan.accessPath(), AccessPath::Intersect);
    ASSERT_TRUE(plan.residual().has_value());
    EXPECT_EQ(predicateKind(*plan.residual()), PredicateKind::Like);
}

TEST(PlannerBehaviorTests, PrefixLikeRequiresWildcardFreeLiteral) {
    // Documented: only lit% with no other wildcards uses ordered prefix LIKE.
    Parser parser;
    auto executor = makeExecutor("prefix-like-wildcards");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(
        executor.execute(parser.parse("CREATE TABLE Employees (id INT, name STRING);")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "INSERT INTO Employees VALUES (1, \"Alice\"), (2, \"Alicia\"), "
                        "(3, \"Bob\");"))
                    .success);
    ASSERT_TRUE(executor.execute(parser.parse("CREATE INDEX idx_name ON Employees(name);")).success);

    auto mixed = executor.execute(
        parser.parse("EXPLAIN SELECT name FROM Employees WHERE name LIKE \"Al_%\";"));
    ASSERT_TRUE(mixed.success);
    EXPECT_EQ(mixed.rows.front().front().toString().find("prefix LIKE"), std::string::npos);
    EXPECT_NE(mixed.rows.front().front().toString().find("full table scan"), std::string::npos);

    auto interior = executor.execute(
        parser.parse("EXPLAIN SELECT name FROM Employees WHERE name LIKE \"A%ce\";"));
    ASSERT_TRUE(interior.success);
    EXPECT_EQ(interior.rows.front().front().toString().find("prefix LIKE"), std::string::npos);
    EXPECT_NE(interior.rows.front().front().toString().find("full table scan"), std::string::npos);

    Table table{"Employees", {{"id", ColumnType::Int}, {"name", ColumnType::String}}};
    table.insert({Value{1}, Value{std::string{"Alice"}}});
    ASSERT_TRUE(table.createIndex("idx_name", "name"));
    Predicate where = LikePred{"name", "Al_%"};
    Select query{"Employees", {}, {SelectExpr::makeStar()}, where, {}, {}};
    const auto plan = QueryPlanner{}.planSelect(query, table);
    EXPECT_EQ(plan.accessPath(), AccessPath::FullScan);
    ASSERT_TRUE(plan.residual().has_value());
    EXPECT_EQ(predicateKind(*plan.residual()), PredicateKind::Like);
}

TEST(PlannerBehaviorTests, TopLevelOrWithNoIndexableDisjunctUsesFullScan) {
    // Documented: when no disjunct is indexable, the planner keeps a full scan.
    // Same-column equality OR rewrites to IN first; without an index that is still a full scan.
    Table table{"Employees", {{"id", ColumnType::Int}, {"name", ColumnType::String}}};
    for (int i = 1; i <= 10; ++i) {
        table.insert({Value{i}, Value{"n" + std::to_string(i)}});
    }
    ASSERT_TRUE(table.createIndex("idx_id", "id"));

    Predicate where =
        makeOr(makeComparison("name", ComparisonOperator::Equal, Value{std::string{"n1"}}),
               makeComparison("name", ComparisonOperator::Equal, Value{std::string{"n2"}}));
    Select query{"Employees", {}, {SelectExpr::makeStar()}, where, {}, {}};
    const auto plan = QueryPlanner{}.planSelect(query, table);
    EXPECT_EQ(plan.accessPath(), AccessPath::FullScan);
    ASSERT_TRUE(plan.residual().has_value());
    EXPECT_NE(plan.estimates.explanation.find("full table scan"), std::string::npos);

    Parser parser;
    auto executor = makeExecutor("or-unindexed-full-scan");
    seedEmployees(executor, parser, true, false);
    auto explain = executor.execute(
        parser.parse("EXPLAIN SELECT name FROM Employees WHERE name = \"Alice\" OR name = \"Bob\";"));
    ASSERT_TRUE(explain.success);
    EXPECT_NE(explain.rows.front().front().toString().find("full table scan"), std::string::npos);
}

} // namespace VertexDB
