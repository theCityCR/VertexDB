#include "test_support.hpp"

#include "VertexDB/execution/prepared_statement_catalog.hpp"
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
    return makeTempExecutor("vertexdb-aggregate-", suffix);
}

} // namespace

TEST(AggregatePreparedTests, PreparedCatalogOwnsAndFindsParsedStatements) {
    PreparedStatementCatalog catalog;
    Query query = Parser{}.parse("SELECT * FROM Employees;");

    catalog.store("ByDepartment", query);

    EXPECT_TRUE(catalog.exists("ByDepartment"));
    EXPECT_FALSE(catalog.exists("bydepartment"));
    EXPECT_TRUE(catalog.find("ByDepartment").has_value());
    EXPECT_FALSE(catalog.find("bydepartment").has_value());
    EXPECT_TRUE(catalog.findCaseInsensitive("bydepartment").has_value());
}

TEST(AggregatePreparedTests, AggregatesAndGroupByProduceGroupedResults) {
    auto executor = makeExecutor("aggregates-groupby");
    Parser parser;
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "CREATE TABLE Employees (id INT, name STRING, salary DOUBLE, dept_id INT);"))
                    .success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "INSERT INTO Employees VALUES (1, \"Alice\", 120000.0, 10), "
                        "(2, \"Bob\", 90000.0, 20), (3, \"Cara\", 110000.0, 10);"))
                    .success);

    auto countAll = executor.execute(parser.parse("SELECT COUNT(*) FROM Employees;"));
    ASSERT_TRUE(countAll.success);
    ASSERT_EQ(countAll.rows.size(), 1U);
    EXPECT_EQ(countAll.rows[0][0], Value{3});
    EXPECT_EQ(countAll.columns[0], "COUNT(*)");

    auto grouped = executor.execute(
        parser.parse("SELECT dept_id, COUNT(*), SUM(salary), AVG(salary), MIN(salary), MAX(salary) "
                     "FROM Employees GROUP BY dept_id ORDER BY dept_id;"));
    ASSERT_TRUE(grouped.success);
    ASSERT_EQ(grouped.rows.size(), 2U);
    EXPECT_EQ(grouped.rows[0][0], Value{10});
    EXPECT_EQ(grouped.rows[0][1], Value{2});
    EXPECT_EQ(grouped.rows[0][2], Value{230000.0});

    auto explain = executor.execute(
        parser.parse("EXPLAIN SELECT dept_id, COUNT(*) FROM Employees GROUP BY dept_id;"));
    ASSERT_TRUE(explain.success);
    bool sawAggregation = false;
    for (const auto &row : explain.rows) {
        if (row.front().toString().find("aggregation") != std::string::npos) {
            sawAggregation = true;
        }
    }
    EXPECT_TRUE(sawAggregation);

    EXPECT_THROW((void)executor.execute(parser.parse("SELECT name, COUNT(*) FROM Employees;")),
                 std::runtime_error);
}

TEST(AggregatePreparedTests, MultiEquiJoinLeftDeepChain) {
    auto executor = makeExecutor("multi-join");
    Parser parser;
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "CREATE TABLE Employees (id INT, name STRING, dept_id INT);"))
                    .success);
    ASSERT_TRUE(
        executor.execute(parser.parse("CREATE TABLE Departments (id INT, office_id INT);")).success);
    ASSERT_TRUE(
        executor.execute(parser.parse("CREATE TABLE Offices (id INT, city STRING);")).success);
    ASSERT_TRUE(
        executor.execute(parser.parse("INSERT INTO Employees VALUES (1, \"Alice\", 10);")).success);
    ASSERT_TRUE(
        executor.execute(parser.parse("INSERT INTO Departments VALUES (10, 100);")).success);
    ASSERT_TRUE(executor.execute(parser.parse("INSERT INTO Offices VALUES (100, \"SF\");")).success);

    auto result = executor.execute(parser.parse(
        "SELECT Employees.name, Offices.city FROM Employees "
        "JOIN Departments ON Employees.dept_id = Departments.id "
        "JOIN Offices ON Departments.office_id = Offices.id;"));
    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.rows.size(), 1U);
    EXPECT_EQ(result.rows[0][0], Value{std::string{"Alice"}});
    EXPECT_EQ(result.rows[0][1], Value{std::string{"SF"}});

    auto explain = executor.execute(parser.parse(
        "EXPLAIN SELECT Employees.name, Offices.city FROM Employees "
        "JOIN Departments ON Employees.dept_id = Departments.id "
        "JOIN Offices ON Departments.office_id = Offices.id;"));
    ASSERT_TRUE(explain.success);
    ASSERT_GE(explain.rows.size(), 2U);
    EXPECT_NE(explain.rows[0].front().toString().find("join"), std::string::npos);
    EXPECT_NE(explain.rows[1].front().toString().find("join"), std::string::npos);
}

TEST(AggregatePreparedTests, PreparedStatementStoresTypedAstWithoutReparse) {
    auto executor = makeExecutor("prepared-ast");
    Parser parser;
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(
        executor.execute(parser.parse("CREATE TABLE Employees (id INT, name STRING);")).success);
    ASSERT_TRUE(
        executor.execute(parser.parse("INSERT INTO Employees VALUES (1, \"Alice\");")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "PREPARE by_id AS \"SELECT name FROM Employees WHERE id = ?;\";"))
                    .success);

    const auto ast = executor.preparedAst("by_id");
    ASSERT_TRUE(ast.has_value());
    ASSERT_TRUE(std::holds_alternative<Select>(*ast));
    const auto &select = std::get<Select>(*ast);
    ASSERT_TRUE(select.where.has_value());
    const auto &comparison = std::get<ComparisonPred>(*select.where);
    EXPECT_TRUE(comparison.value.isParameter());
    EXPECT_EQ(comparison.value.parameterIndex(), 0U);

    auto result = executor.execute(parser.parse("EXECUTE by_id VALUES (1);"));
    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.rows.size(), 1U);
    EXPECT_EQ(result.rows[0][0], Value{std::string{"Alice"}});
}

TEST(AggregatePreparedTests, ExecuteDoesNotMutateStoredPreparedAst) {
    // Desired: EXECUTE binds a clone; catalog still holds ? parameter slots afterward.
    auto executor = makeExecutor("prepared-immutable");
    Parser parser;
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(
        executor.execute(parser.parse("CREATE TABLE Employees (id INT, name STRING);")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "INSERT INTO Employees VALUES (1, \"Alice\"), (2, \"Bob\");"))
                    .success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "PREPARE by_id AS \"SELECT name FROM Employees WHERE id = ?;\";"))
                    .success);

    ASSERT_TRUE(executor.execute(parser.parse("EXECUTE by_id VALUES (1);")).success);
    ASSERT_TRUE(executor.execute(parser.parse("EXECUTE by_id VALUES (2);")).success);

    const auto ast = executor.preparedAst("by_id");
    ASSERT_TRUE(ast.has_value());
    ASSERT_TRUE(std::holds_alternative<Select>(*ast));
    const auto &select = std::get<Select>(*ast);
    ASSERT_TRUE(select.where.has_value());
    const auto &comparison = std::get<ComparisonPred>(*select.where);
    EXPECT_TRUE(comparison.value.isParameter()) << comparison.value.toString();
    EXPECT_EQ(comparison.value.parameterIndex(), 0U);

    auto again = executor.execute(parser.parse("EXECUTE by_id VALUES (1);"));
    ASSERT_TRUE(again.success);
    ASSERT_EQ(again.rows.size(), 1U);
    EXPECT_EQ(again.rows[0][0], Value{std::string{"Alice"}});
}

TEST(AggregatePreparedTests, GroupByNullKeyFormsSingleGroup) {
    // Intentional VertexDB semantics: NULL group keys hash together (Value equality).
    auto executor = makeExecutor("groupby-null");
    Parser parser;
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "CREATE TABLE Employees (id INT, dept INT NULL, salary DOUBLE);"))
                    .success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "INSERT INTO Employees VALUES (1, 10, 100.0), (2, NULL, 50.0), "
                        "(3, NULL, 25.0), (4, 10, 20.0);"))
                    .success);

    auto result = executor.execute(parser.parse(
        "SELECT dept, COUNT(*), SUM(salary) FROM Employees GROUP BY dept ORDER BY dept;"));
    ASSERT_TRUE(result.success) << result.message;
    ASSERT_EQ(result.rows.size(), 2U);
    // NULL sorts before INT under VertexDB Value ordering (monostate index).
    EXPECT_TRUE(result.rows[0][0].isNull());
    EXPECT_EQ(result.rows[0][1], Value{static_cast<std::int64_t>(2)});
    EXPECT_EQ(result.rows[0][2], Value{75.0});
    EXPECT_EQ(result.rows[1][0], Value{static_cast<std::int64_t>(10)});
    EXPECT_EQ(result.rows[1][1], Value{static_cast<std::int64_t>(2)});
    EXPECT_EQ(result.rows[1][2], Value{120.0});
}

TEST(AggregatePreparedTests, AnalyzeBuildsEquiHeightHistogramsWithDistinctCounts) {
    Table table{"Employees", {{"id", ColumnType::Int}, {"dept", ColumnType::Int}}};
    for (int i = 1; i <= 64; ++i) {
        table.insert({Value{i}, Value{i % 4}});
    }
    table.analyze();
    const auto idHist = table.columnHistogram("id");
    ASSERT_TRUE(idHist.has_value());
    EXPECT_EQ(idHist->rowCount, 64U);
    EXPECT_EQ(idHist->distinctCount, 64U);
    EXPECT_EQ(idHist->buckets.size(), kDefaultHistogramBuckets);
    const auto deptHist = table.columnHistogram("dept");
    ASSERT_TRUE(deptHist.has_value());
    EXPECT_EQ(deptHist->distinctCount, 4U);
    EXPECT_FALSE(deptHist->buckets.empty());
}

TEST(AggregatePreparedTests, AggregateAvgMinMaxAndCountColumnSkipNulls) {
    auto executor = makeExecutor("agg-nulls-avg");
    Parser parser;
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "CREATE TABLE Employees (id INT, name STRING NULL, salary DOUBLE NULL);"))
                    .success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "INSERT INTO Employees VALUES (1, \"Alice\", 100.0), (2, NULL, 200.0), "
                        "(3, \"Cara\", NULL);"))
                    .success);

    auto empty = makeExecutor("agg-empty");
    ASSERT_TRUE(empty.execute(parser.parse("CREATE DATABASE empty;")).success);
    ASSERT_TRUE(empty.execute(parser.parse("CREATE TABLE T (id INT);")).success);
    auto emptyCount = empty.execute(parser.parse("SELECT COUNT(*) FROM T;"));
    ASSERT_TRUE(emptyCount.success);
    ASSERT_EQ(emptyCount.rows.size(), 1U);
    EXPECT_EQ(emptyCount.rows[0][0], Value{0});

    auto result = executor.execute(
        parser.parse("SELECT COUNT(*), COUNT(name), AVG(salary), MIN(salary), MAX(salary) "
                     "FROM Employees;"));
    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.rows.size(), 1U);
    EXPECT_EQ(result.rows[0][0], Value{3});
    EXPECT_EQ(result.rows[0][1], Value{2}); // NULL name skipped
    EXPECT_EQ(result.rows[0][2], Value{150.0});
    EXPECT_EQ(result.rows[0][3], Value{100.0});
    EXPECT_EQ(result.rows[0][4], Value{200.0});

    EXPECT_THROW((void)executor.execute(parser.parse("SELECT * FROM Employees GROUP BY id;")),
                 std::runtime_error);
}

TEST(AggregatePreparedTests, PreparedMultiParamBindAndArityRejection) {
    auto executor = makeExecutor("prepared-arity");
    Parser parser;
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "CREATE TABLE Employees (id INT, name STRING, salary DOUBLE);"))
                    .success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "INSERT INTO Employees VALUES (1, \"Alice\", 120000.0), (2, \"Bob\", "
                        "90000.0);"))
                    .success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "PREPARE by_id_salary AS \"SELECT name FROM Employees WHERE id = ? AND "
                        "salary = ?;\";"))
                    .success);

    const auto ast = executor.preparedAst("by_id_salary");
    ASSERT_TRUE(ast.has_value());
    ASSERT_TRUE(std::holds_alternative<Select>(*ast));
    const auto &select = std::get<Select>(*ast);
    ASSERT_TRUE(select.where.has_value());
    EXPECT_EQ(predicateKind(*select.where), PredicateKind::And);
    const auto &andPred = std::get<AndPred>(*select.where);
    ASSERT_TRUE(andPred.left);
    ASSERT_TRUE(andPred.right);
    const auto &left = std::get<ComparisonPred>(*andPred.left);
    const auto &right = std::get<ComparisonPred>(*andPred.right);
    EXPECT_TRUE(left.value.isParameter());
    EXPECT_TRUE(right.value.isParameter());
    EXPECT_EQ(left.value.parameterIndex(), 0U);
    EXPECT_EQ(right.value.parameterIndex(), 1U);

    auto ok = executor.execute(parser.parse("EXECUTE by_id_salary VALUES (1, 120000.0);"));
    ASSERT_TRUE(ok.success);
    ASSERT_EQ(ok.rows.size(), 1U);
    EXPECT_EQ(ok.rows[0][0], Value{std::string{"Alice"}});

    EXPECT_THROW((void)executor.execute(parser.parse("EXECUTE by_id_salary VALUES (1);")),
                 std::runtime_error);
    EXPECT_THROW(
        (void)executor.execute(parser.parse("EXECUTE by_id_salary VALUES (1, 120000.0, 3);")),
        std::runtime_error);
}

TEST(AggregatePreparedTests, AnalyzeWithoutTableNameAnalyzesAllTables) {
    auto executor = makeExecutor("analyze-all");
    Parser parser;
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(executor.execute(parser.parse("CREATE TABLE A (id INT);")).success);
    ASSERT_TRUE(executor.execute(parser.parse("CREATE TABLE B (score INT);")).success);
    ASSERT_TRUE(executor.execute(parser.parse("INSERT INTO A VALUES (1), (2);")).success);
    ASSERT_TRUE(executor.execute(parser.parse("INSERT INTO B VALUES (10), (20), (30);")).success);

    auto result = executor.execute(parser.parse("ANALYZE;"));
    ASSERT_TRUE(result.success);
    EXPECT_NE(result.message.find("2 table"), std::string::npos);

    auto tableA = executor.currentDatabase()->table("A");
    auto tableB = executor.currentDatabase()->table("B");
    ASSERT_TRUE(tableA != nullptr);
    ASSERT_TRUE(tableB != nullptr);
    ASSERT_TRUE(tableA->columnHistogram("id").has_value());
    ASSERT_TRUE(tableB->columnHistogram("score").has_value());
    EXPECT_EQ(tableA->columnHistogram("id")->distinctCount, 2U);
    EXPECT_EQ(tableB->columnHistogram("score")->distinctCount, 3U);
}

TEST(AggregatePreparedTests, GroupByOrderByLimitAppliesToAggregateGroups) {
    auto executor = makeExecutor("agg-order-limit");
    Parser parser;
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "CREATE TABLE Employees (id INT, dept_id INT, salary DOUBLE);"))
                    .success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "INSERT INTO Employees VALUES (1, 10, 100.0), (2, 10, 200.0), "
                        "(3, 20, 50.0), (4, 30, 500.0);"))
                    .success);

    auto result = executor.execute(
        parser.parse("SELECT dept_id, SUM(salary) FROM Employees GROUP BY dept_id "
                     "ORDER BY dept_id DESC LIMIT 2;"));
    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.rows.size(), 2U);
    EXPECT_EQ(result.rows[0][0], Value{30});
    EXPECT_EQ(result.rows[0][1], Value{500.0});
    EXPECT_EQ(result.rows[1][0], Value{20});
    EXPECT_EQ(result.rows[1][1], Value{50.0});
}

TEST(AggregatePreparedTests, MixedNumericAggregatesAndEmptySumAvg) {
    auto executor = makeExecutor("mixed-numeric-agg");
    Parser parser;
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "CREATE TABLE Metrics (id INT, score INT, weight DOUBLE NULL);"))
                    .success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "INSERT INTO Metrics VALUES (1, 10, 1.5), (2, 20, 2.5), (3, 30, NULL);"))
                    .success);

    auto mixed = executor.execute(
        parser.parse("SELECT SUM(score), AVG(score), SUM(weight), AVG(weight) FROM Metrics;"));
    ASSERT_TRUE(mixed.success);
    ASSERT_EQ(mixed.rows.size(), 1U);
    EXPECT_EQ(mixed.rows[0][0], Value{60});
    EXPECT_EQ(mixed.rows[0][1], Value{20.0});
    EXPECT_EQ(mixed.rows[0][2], Value{4.0});
    EXPECT_EQ(mixed.rows[0][3], Value{2.0});

    auto filtered = executor.execute(
        parser.parse("SELECT COUNT(*), SUM(weight) FROM Metrics WHERE id > 100;"));
    ASSERT_TRUE(filtered.success);
    ASSERT_EQ(filtered.rows.size(), 1U);
    EXPECT_EQ(filtered.rows[0][0], Value{0});
    EXPECT_TRUE(filtered.rows[0][1].isNull());
}

TEST(AggregatePreparedTests, PreparedInListAndUpdateParameters) {
    auto executor = makeExecutor("prepared-in-update");
    Parser parser;
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "CREATE TABLE Employees (id INT, name STRING, salary DOUBLE);"))
                    .success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "INSERT INTO Employees VALUES (1, \"Alice\", 100.0), (2, \"Bob\", 200.0), "
                        "(3, \"Cara\", 300.0);"))
                    .success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "PREPARE by_name AS \"SELECT id FROM Employees WHERE name = ?;\";"))
                    .success);
    auto selected = executor.execute(parser.parse("EXECUTE by_name VALUES (\"Bob\");"));
    ASSERT_TRUE(selected.success);
    ASSERT_EQ(selected.rows.size(), 1U);
    EXPECT_EQ(selected.rows[0][0], Value{2});

    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "PREPARE bump AS \"UPDATE Employees SET salary = ? WHERE id = ?;\";"))
                    .success);
    ASSERT_TRUE(executor.execute(parser.parse("EXECUTE bump VALUES (999.0, 2);")).success);
    auto bob = executor.execute(parser.parse("SELECT salary FROM Employees WHERE id = 2;"));
    ASSERT_TRUE(bob.success);
    ASSERT_EQ(bob.rows.size(), 1U);
    EXPECT_EQ(bob.rows[0][0], Value{999.0});

    EXPECT_THROW((void)executor.execute(parser.parse("EXECUTE missing VALUES (1);")),
                 std::runtime_error);
}

TEST(AggregatePreparedTests, ParsesCreateDatabaseIndexSaveLoadAndExit) {
    Parser parser;

    auto createDb = parser.parse("CREATE DATABASE company;");
    ASSERT_TRUE(std::holds_alternative<CreateDatabase>(createDb));
    EXPECT_EQ(std::get<CreateDatabase>(createDb).name, "company");

    auto createIndex = parser.parse("CREATE INDEX idx_id ON Employees(id);");
    ASSERT_TRUE(std::holds_alternative<CreateIndex>(createIndex));
    EXPECT_EQ(std::get<CreateIndex>(createIndex).name, "idx_id");
    EXPECT_EQ(std::get<CreateIndex>(createIndex).table, "Employees");
    EXPECT_EQ(std::get<CreateIndex>(createIndex).column, "id");

    EXPECT_TRUE(std::holds_alternative<SaveDatabase>(parser.parse("SAVE DATABASE;")));
    auto loadNamed = parser.parse("LOAD DATABASE company;");
    ASSERT_TRUE(std::holds_alternative<LoadDatabase>(loadNamed));
    ASSERT_TRUE(std::get<LoadDatabase>(loadNamed).name.has_value());
    EXPECT_EQ(*std::get<LoadDatabase>(loadNamed).name, "company");
    auto loadActive = parser.parse("LOAD DATABASE;");
    ASSERT_TRUE(std::holds_alternative<LoadDatabase>(loadActive));
    EXPECT_FALSE(std::get<LoadDatabase>(loadActive).name.has_value());
    EXPECT_TRUE(std::holds_alternative<Exit>(parser.parse("EXIT;")));
}

TEST(AggregatePreparedTests, SqlLiteralHelpersCoverNullDoubleAndEscapes) {
    EXPECT_EQ(sqlLiteral(Value{}), "NULL");
    EXPECT_EQ(sqlLiteral(Value{42}), "42");
    EXPECT_EQ(sqlLiteral(Value{3.0}), "3.0");
    EXPECT_EQ(sqlLiteral(Value{std::string{"a\"b\\c"}}), "\"a\\\"b\\\\c\"");

    Predicate comparison = makeComparison("id", ComparisonOperator::Equal, Value{1});
    EXPECT_NE(predicateLiteral(comparison).find("id"), std::string::npos);
    Predicate greater = makeComparison("salary", ComparisonOperator::Greater, Value{1.0});
    EXPECT_NE(predicateLiteral(greater).find(">"), std::string::npos);
    Predicate less = makeComparison("salary", ComparisonOperator::Less, Value{1.0});
    EXPECT_NE(predicateLiteral(less).find("<"), std::string::npos);

    IndexExpression exprCmp;
    exprCmp.kind = IndexExpression::Kind::Negate;
    exprCmp.column = "salary";
    auto exprPred = makeExpressionComparison(exprCmp, ComparisonOperator::Equal, Value{-1.0});
    EXPECT_NE(predicateLiteral(exprPred).find("-salary"), std::string::npos);

    Predicate withRhs = comparison;
    std::get<ComparisonPred>(withRhs).rhsColumn = "other.id";
    EXPECT_NE(predicateLiteral(withRhs).find("other.id"), std::string::npos);

    Predicate inList = makeInList("id", {Value{1}, Value{2}});
    EXPECT_NE(predicateLiteral(inList).find("IN"), std::string::npos);

    Select sub;
    sub.table = "Employees";
    sub.columns = {SelectExpr::makeColumn("id")};
    Predicate inSub = makeInSubquery("id", std::make_shared<Select>(sub));
    EXPECT_NE(predicateLiteral(inSub).find("IN (SELECT"), std::string::npos);
    EXPECT_NE(predicateLiteral(makeExists(std::make_shared<Select>(sub))).find("EXISTS"),
              std::string::npos);

    Predicate both = makeAnd(comparison, inList);
    EXPECT_NE(predicateLiteral(both).find("AND"), std::string::npos);

    CreateIndex index{"idx_id", "Employees", "id"};
    EXPECT_NE(createIndexSql(index).find("CREATE INDEX"), std::string::npos);

    IndexExpression expr;
    expr.kind = IndexExpression::Kind::Negate;
    expr.column = "salary";
    CreateIndex exprIndex{"idx_neg", "Employees", {}};
    exprIndex.expression = expr;
    EXPECT_NE(createIndexSql(exprIndex).find("-salary"), std::string::npos);

    Row row{Value{1}, Value{std::string{"Alice"}}, Value{}};
    EXPECT_NE(insertSql("Employees", row).find("INSERT INTO Employees"), std::string::npos);
    Update update{"Employees", "name", Value{std::string{"Bob"}}, comparison};
    EXPECT_NE(updateSql(update).find("UPDATE Employees"), std::string::npos);
    Delete del{"Employees", comparison};
    EXPECT_NE(deleteSql(del).find("DELETE FROM Employees"), std::string::npos);
}

TEST(AggregatePreparedTests, PreparedBindCoversExplainDeleteInsertAndCte) {
    Parser parser;
    auto executor = makeExecutor("prep-bind-paths");
    seedEmployees(executor, parser, true, true);

    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "PREPARE del AS \"DELETE FROM Employees WHERE id = ?;\";"))
                    .success);
    ASSERT_TRUE(executor.execute(parser.parse("EXECUTE del VALUES (2);")).success);

    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "PREPARE ins AS \"INSERT INTO Employees VALUES (?, ?, ?);\";"))
                    .success);
    ASSERT_TRUE(
        executor.execute(parser.parse("EXECUTE ins VALUES (9, \"Zed\", 1.0);")).success);

    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "PREPARE ex AS \"EXPLAIN SELECT name FROM Employees WHERE id = ?;\";"))
                    .success);
    auto explained = executor.execute(parser.parse("EXECUTE ex VALUES (1);"));
    ASSERT_TRUE(explained.success);
    EXPECT_FALSE(explained.rows.empty());
    EXPECT_EQ(explained.rows.front().front().toString().find("actual_rows="), std::string::npos);

    ASSERT_TRUE(executor
                    .execute(parser.parse("PREPARE exa AS \"EXPLAIN ANALYZE SELECT name FROM "
                                          "Employees WHERE id = ?;\";"))
                    .success);
    auto analyzed = executor.execute(parser.parse("EXECUTE exa VALUES (1);"));
    ASSERT_TRUE(analyzed.success);
    ASSERT_FALSE(analyzed.rows.empty());
    const auto analyzeText = analyzed.rows.front().front().toString();
    EXPECT_NE(analyzeText.find("actual_rows=1"), std::string::npos);
    EXPECT_NE(analyzeText.find("actual_time_ms="), std::string::npos);

    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "PREPARE cte AS \"WITH high AS (SELECT id, name FROM Employees WHERE "
                        "id = ?) SELECT name FROM high;\";"))
                    .success);
    auto cte = executor.execute(parser.parse("EXECUTE cte VALUES (1);"));
    ASSERT_TRUE(cte.success);
    ASSERT_EQ(cte.rows.size(), 1U);

    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "PREPARE idx AS \"CREATE INDEX idx_neg ON Employees((-salary));\";"))
                    .success);
    ASSERT_TRUE(executor.execute(parser.parse("EXECUTE idx;")).success);
}

} // namespace VertexDB
