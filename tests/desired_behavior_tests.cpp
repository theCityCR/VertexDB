#include "VertexDB/execution/query_executor.hpp"
#include "VertexDB/indexing/btree_index.hpp"
#include "VertexDB/parser/parser.hpp"
#include "VertexDB/persistence/write_ahead_log.hpp"
#include "VertexDB/planner/query_planner.hpp"
#include "VertexDB/planner/rewriter.hpp"
#include "VertexDB/storage/row_store.hpp"
#include "VertexDB/storage/table.hpp"

#include <gtest/gtest.h>

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
        std::filesystem::temp_directory_path() / ("vertexdb-desired-" + std::string(suffix));
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

// --- P0 -----------------------------------------------------------------

TEST(DesiredBehaviorTests, ResidualFilterRejectsIndexedHitsThatFailRemainingConjuncts) {
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

TEST(DesiredBehaviorTests, ExplainReportsJoinPlanFromCostModel) {
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

TEST(DesiredBehaviorTests, NestedSqlDocumentedRefusalsAreRejected) {
    Parser parser;

    EXPECT_THROW(
        (void)parser.parse("WITH cte AS (SELECT id FROM Employees JOIN Departments ON dept_id = id) "
                           "SELECT id FROM cte;"),
        std::runtime_error);

    EXPECT_THROW((void)parser.parse("WITH outer_cte AS (WITH inner_cte AS (SELECT id FROM Employees) "
                                    "SELECT id FROM inner_cte) SELECT id FROM outer_cte;"),
                 std::runtime_error);

    auto query = parser.parse(
        "WITH high AS (SELECT id, name FROM Employees WHERE id = 1) "
        "SELECT * FROM high JOIN Departments ON id = id;");
    ASSERT_TRUE(std::holds_alternative<Select>(query));
    EXPECT_THROW((void)rewriteSelect(std::get<Select>(query)), std::runtime_error);
}

TEST(DesiredBehaviorTests, CommitPersistsTransactionMutations) {
    Parser parser;
    auto executor = makeExecutor("commit");
    seedEmployees(executor, parser, true, false);

    ASSERT_TRUE(executor.execute(parser.parse("BEGIN;")).success);
    ASSERT_TRUE(
        executor.execute(parser.parse("UPDATE Employees SET salary = 999999.0 WHERE id = 1;"))
            .success);
    ASSERT_TRUE(executor.execute(parser.parse("COMMIT;")).success);

    auto result =
        executor.execute(parser.parse("SELECT salary FROM Employees WHERE id = 1;"));
    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.rows.size(), 1U);
    EXPECT_EQ(result.rows[0][0], Value{999999.0});

    auto doubleCommit = executor.execute(parser.parse("COMMIT;"));
    EXPECT_FALSE(doubleCommit.success);
}

TEST(DesiredBehaviorTests, RollbackKeepsSameDatabaseInstance) {
    Parser parser;
    auto executor = makeExecutor("undo-identity");
    seedEmployees(executor, parser, true, false);

    const auto databaseBefore = executor.currentDatabase();
    ASSERT_NE(databaseBefore, nullptr);

    ASSERT_TRUE(executor.execute(parser.parse("BEGIN;")).success);
    ASSERT_TRUE(
        executor.execute(parser.parse("INSERT INTO Employees VALUES (99, \"Zed\", 1.0);")).success);
    ASSERT_TRUE(executor.execute(parser.parse("ROLLBACK;")).success);

    EXPECT_EQ(executor.currentDatabase(), databaseBefore);
    auto result = executor.execute(parser.parse("SELECT id FROM Employees WHERE id = 99;"));
    ASSERT_TRUE(result.success);
    EXPECT_TRUE(result.rows.empty());
}

TEST(DesiredBehaviorTests, RollbackReversesMixedDmlAndIndexedLookups) {
    Parser parser;
    auto executor = makeExecutor("undo-mixed");
    seedEmployees(executor, parser, true, false);

    auto before = executor.execute(parser.parse("SELECT id, name, salary FROM Employees ORDER BY id;"));
    ASSERT_TRUE(before.success);
    ASSERT_EQ(before.rows.size(), 3U);

    ASSERT_TRUE(executor.execute(parser.parse("BEGIN;")).success);
    ASSERT_TRUE(
        executor.execute(parser.parse("INSERT INTO Employees VALUES (4, \"Dana\", 80000.0);"))
            .success);
    ASSERT_TRUE(
        executor.execute(parser.parse("UPDATE Employees SET name = \"Alicia\" WHERE id = 1;"))
            .success);
    ASSERT_TRUE(executor.execute(parser.parse("DELETE FROM Employees WHERE id = 2;")).success);
    ASSERT_TRUE(executor.execute(parser.parse("ROLLBACK;")).success);

    auto after = executor.execute(parser.parse("SELECT id, name, salary FROM Employees ORDER BY id;"));
    ASSERT_TRUE(after.success);
    ASSERT_EQ(after.rows.size(), before.rows.size());
    EXPECT_EQ(after.rows, before.rows);

    auto bob = executor.execute(parser.parse("SELECT name FROM Employees WHERE id = 2;"));
    ASSERT_TRUE(bob.success);
    ASSERT_EQ(bob.rows.size(), 1U);
    EXPECT_EQ(bob.rows.front().front(), Value{std::string{"Bob"}});
}

TEST(DesiredBehaviorTests, SchemaChangesRejectedWhileTransactionActive) {
    Parser parser;
    auto executor = makeExecutor("undo-ddl");
    seedEmployees(executor, parser, true, false);

    ASSERT_TRUE(executor.execute(parser.parse("BEGIN;")).success);
    auto create = executor.execute(parser.parse("CREATE TABLE Other (id INT);"));
    EXPECT_FALSE(create.success);
    EXPECT_NE(create.message.find("not allowed while a transaction is active"), std::string::npos);

    auto index = executor.execute(parser.parse("CREATE INDEX idx_name ON Employees(name);"));
    EXPECT_FALSE(index.success);

    ASSERT_TRUE(executor.execute(parser.parse("ROLLBACK;")).success);
    auto after = executor.execute(parser.parse("CREATE TABLE Other (id INT);"));
    EXPECT_TRUE(after.success);
}

// --- P1 -----------------------------------------------------------------

TEST(DesiredBehaviorTests, PageRowStoreMirrorsSerializedPagesIntoBufferPool) {
    PageRowStore store{2, 4};

    const auto first = store.append({Value{1}, Value{std::string{"Alice"}}});
    const auto pageId = store.pageIdFor(first);
    EXPECT_TRUE(store.bufferContains(pageId));
    EXPECT_GE(store.bufferSize(), 1U);

    ASSERT_TRUE(store.update(first, {Value{11}, Value{std::string{"Alicia"}}}));
    EXPECT_TRUE(store.bufferContains(pageId));

    const auto second = store.append({Value{2}, Value{std::string{"Bob"}}});
    const auto third = store.append({Value{3}, Value{std::string{"Cara"}}});
    EXPECT_EQ(store.pageIdFor(second), pageId);
    EXPECT_NE(store.pageIdFor(third), pageId);
    EXPECT_TRUE(store.bufferContains(store.pageIdFor(third)));

    ASSERT_TRUE(store.erase(first));
    EXPECT_TRUE(store.bufferContains(pageId));
}

TEST(DesiredBehaviorTests, PageRowStoreReadsLiveRowsFromPagePayloadBytes) {
    // Tiny buffer (capacity 1) so accessing one page evicts the other from the LRU cache.
    constexpr std::size_t rowsPerPage = 2;
    PageRowStore store{rowsPerPage, 1};

    const auto first = store.append({Value{1}, Value{std::string{"Alice"}}});
    const auto second = store.append({Value{2}, Value{std::string{"Bob"}}});
    const auto third = store.append({Value{3}, Value{std::string{"Cara"}}});
    ASSERT_TRUE(store.update(second, {Value{20}, Value{std::string{"Bobby"}}}));
    ASSERT_TRUE(store.erase(first));
    const auto reused = store.append({Value{4}, Value{std::string{"Dana"}}});
    EXPECT_EQ(reused, first);

    const auto page1 = store.pageIdFor(second);
    const auto page2 = store.pageIdFor(third);
    EXPECT_EQ(page1, store.pageIdFor(reused));
    EXPECT_NE(page1, page2);

    // Touch page 2 so the capacity-1 pool evicts page 1.
    ASSERT_NE(store.get(third), nullptr);
    EXPECT_FALSE(store.bufferContains(page1));
    EXPECT_TRUE(store.bufferContains(page2));

    auto expectMatchesPayload = [&](RowId rowId, const Row &expected) {
        const auto *row = store.get(rowId);
        ASSERT_NE(row, nullptr);
        EXPECT_EQ(*row, expected);

        const auto pageId = store.pageIdFor(rowId);
        const auto bytes = store.directoryBytes(pageId);
        ASSERT_TRUE(bytes.has_value());
        const auto decoded = PageRowStore::decodePage(*bytes);
        const auto offset = rowId % rowsPerPage;
        ASSERT_LT(offset, decoded.size());
        EXPECT_EQ(decoded[offset], expected);
        // Fill-on-miss: reading reloads the durable page bytes into the buffer pool.
        EXPECT_TRUE(store.bufferContains(pageId));
    };

    expectMatchesPayload(reused, {Value{4}, Value{std::string{"Dana"}}});
    expectMatchesPayload(second, {Value{20}, Value{std::string{"Bobby"}}});
    expectMatchesPayload(third, {Value{3}, Value{std::string{"Cara"}}});
}

TEST(DesiredBehaviorTests, PlannerAndExplainUseOrderedIndexForLessThan) {
    Table table{"Employees", {{"id", ColumnType::Int}, {"salary", ColumnType::Double}}};
    table.insert({Value{1}, Value{120000.0}});
    table.insert({Value{2}, Value{90000.0}});
    ASSERT_TRUE(table.createIndex("idx_salary", "salary"));

    QueryPlanner planner;
    Select less{"Employees", std::nullopt,
                {"*"},       Predicate{"salary", ComparisonOperator::Less, Value{100000.0}},
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

TEST(DesiredBehaviorTests, OrPredicateFullScanIsDocumentedLimitation) {
    // Intentional v1 limitation (docs/sql.md): OR is not split for indexing.
    Table table{"Employees", {{"id", ColumnType::Int}}};
    table.insert({Value{1}});
    table.insert({Value{2}});
    ASSERT_TRUE(table.createIndex("idx_id", "id"));

    Predicate orPredicate{
        Predicate::Kind::Or,
        std::make_shared<Predicate>(Predicate{"id", ComparisonOperator::Equal, Value{1}}),
        std::make_shared<Predicate>(Predicate{"id", ComparisonOperator::Equal, Value{2}})};
    Select query{"Employees", std::nullopt, {"*"}, orPredicate, {}, {}};
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

TEST(DesiredBehaviorTests, InSubqueryFallsBackToScanWhenOuterColumnUnindexed) {
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

TEST(DesiredBehaviorTests, CteInliningLeavesBodyFilterAsResidualWhenOuterUsesIndex) {
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

TEST(DesiredBehaviorTests, ScaledCteWinQueryUsesHashIndexAndResidual) {
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

// --- P2 -----------------------------------------------------------------

TEST(DesiredBehaviorTests, ParsesCreateDatabaseIndexSaveLoadAndExit) {
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

TEST(DesiredBehaviorTests, MultiCteInliningAndUnusedCteNote) {
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

TEST(DesiredBehaviorTests, IndexPreferredOverFullScanWhenCostsAreTied) {
    Table table{"Employees", {{"id", ColumnType::Int}}};
    table.insert({Value{1}});
    ASSERT_TRUE(table.createIndex("idx_id", "id"));

    Select query{"Employees", std::nullopt,
                 {"*"},       Predicate{"id", ComparisonOperator::Equal, Value{1}},
                 {},          {}};
    const auto plan = QueryPlanner{}.planSelect(query, table);
    EXPECT_EQ(plan.accessPath, AccessPath::HashIndexLookup);
}

TEST(DesiredBehaviorTests, MultiConjunctAndPicksCheapestIndexableAndKeepsResidualTree) {
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

    Select query{"Employees", std::nullopt, {"*"}, where, {}, {}};
    const auto plan = QueryPlanner{}.planSelect(query, table);
    EXPECT_EQ(plan.accessPath, AccessPath::HashIndexLookup);
    EXPECT_EQ(plan.indexColumn, "id");
    ASSERT_TRUE(plan.residual.has_value());
    EXPECT_EQ(plan.residual->kind, Predicate::Kind::And);
}

TEST(DesiredBehaviorTests, ExplainReportsNoResidualForPureEqualityIndexLookup) {
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

TEST(DesiredBehaviorTests, UpdateAndDeleteUseFullScanEvenWhenPredicateColumnIndexed) {
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

TEST(DesiredBehaviorTests, UncommittedWritesInvisibleUntilCommit) {
    TransactionManager transactions;
    Table table{"Employees",
                {{"id", ColumnType::Int}, {"name", ColumnType::String}, {"salary", ColumnType::Double}}};

    const auto baseline = transactions.beginCommitted();
    table.insert({Value{static_cast<std::int64_t>(1)}, Value{"Alice"}, Value{120000.0}}, baseline);

    const auto writer = transactions.begin();
    table.insert({Value{static_cast<std::int64_t>(99)}, Value{"Zed"}, Value{1.0}}, writer.id);

    auto visible = table.rowsSnapshot(transactions.currentSnapshot(), transactions);
    ASSERT_EQ(visible.size(), 1U);
    EXPECT_EQ(visible.front()[0], Value{static_cast<std::int64_t>(1)});

    transactions.commit(writer.id);
    auto afterCommit = table.rowsSnapshot(transactions.currentSnapshot(), transactions);
    ASSERT_EQ(afterCommit.size(), 2U);
}

TEST(DesiredBehaviorTests, SnapshotIsolationHidesCommitsAfterBegin) {
    TransactionManager transactions;
    Table table{"Employees",
                {{"id", ColumnType::Int}, {"name", ColumnType::String}, {"salary", ColumnType::Double}}};

    table.insert({Value{static_cast<std::int64_t>(1)}, Value{"Alice"}, Value{120000.0}},
                 transactions.beginCommitted());

    const auto reader = transactions.begin();
    const auto snap = transactions.currentSnapshot(reader.id);
    EXPECT_EQ(table.rowsSnapshot(snap, transactions).size(), 1U);

    const auto concurrent = transactions.begin();
    table.insert({Value{static_cast<std::int64_t>(99)}, Value{"Zed"}, Value{1.0}}, concurrent.id);
    transactions.commit(concurrent.id);

    auto during = table.rowsSnapshot(snap, transactions);
    ASSERT_EQ(during.size(), 1U);
    EXPECT_EQ(during.front()[0], Value{static_cast<std::int64_t>(1)});

    auto latest = table.rowsSnapshot(transactions.currentSnapshot(), transactions);
    ASSERT_EQ(latest.size(), 2U);
}

TEST(DesiredBehaviorTests, TransactionReadsOwnUncommittedWrites) {
    Parser parser;
    auto executor = makeExecutor("read-own-writes");
    seedEmployees(executor, parser, false, false);

    ASSERT_TRUE(executor.execute(parser.parse("BEGIN;")).success);
    ASSERT_TRUE(
        executor.execute(parser.parse("INSERT INTO Employees VALUES (99, \"Zed\", 1.0);")).success);
    auto own = executor.execute(parser.parse("SELECT name FROM Employees WHERE id = 99;"));
    ASSERT_TRUE(own.success);
    ASSERT_EQ(own.rows.size(), 1U);
    EXPECT_EQ(own.rows[0][0], Value{"Zed"});
    ASSERT_TRUE(executor.execute(parser.parse("ROLLBACK;")).success);
}

TEST(DesiredBehaviorTests, RollbackDropsDeferredWalRecords) {
    const auto root =
        std::filesystem::temp_directory_path() / "vertexdb-desired-wal-rollback-atomic";
    std::filesystem::remove_all(root);
    Parser parser;
    QueryExecutor executor{root};

    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(
        executor
            .execute(parser.parse("CREATE TABLE Employees (id INT, name STRING, salary DOUBLE);"))
            .success);
    ASSERT_TRUE(executor
                    .execute(parser.parse("INSERT INTO Employees VALUES (1, \"Alice\", 120000.0);"))
                    .success);

    const auto before = WriteAheadLog{root / "VertexDB.wal"}.readAll();
    ASSERT_EQ(before.size(), 3U);

    ASSERT_TRUE(executor.execute(parser.parse("BEGIN;")).success);
    ASSERT_TRUE(
        executor.execute(parser.parse("INSERT INTO Employees VALUES (99, \"Zed\", 1.0);")).success);
    ASSERT_TRUE(
        executor.execute(parser.parse("UPDATE Employees SET salary = 2.0 WHERE id = 1;")).success);
    ASSERT_TRUE(executor.execute(parser.parse("DELETE FROM Employees WHERE id = 1;")).success);

    EXPECT_EQ(WriteAheadLog{root / "VertexDB.wal"}.readAll().size(), before.size());

    ASSERT_TRUE(executor.execute(parser.parse("ROLLBACK;")).success);

    const auto after = WriteAheadLog{root / "VertexDB.wal"}.readAll();
    ASSERT_EQ(after.size(), before.size());
    for (std::size_t i = 0; i < before.size(); ++i) {
        EXPECT_EQ(after[i].operation, before[i].operation);
        EXPECT_EQ(after[i].payload, before[i].payload);
    }

    QueryExecutor recovered{root};
    auto result = recovered.execute(parser.parse("SELECT id FROM Employees WHERE id = 99;"));
    ASSERT_TRUE(result.success);
    EXPECT_TRUE(result.rows.empty());
    std::filesystem::remove_all(root);
}

TEST(DesiredBehaviorTests, CommitFlushesDeferredWalAndRecovers) {
    const auto root =
        std::filesystem::temp_directory_path() / "vertexdb-desired-wal-commit-atomic";
    std::filesystem::remove_all(root);
    Parser parser;

    {
        QueryExecutor executor{root};
        ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
        ASSERT_TRUE(executor
                        .execute(parser.parse(
                            "CREATE TABLE Employees (id INT, name STRING, salary DOUBLE);"))
                        .success);

        ASSERT_TRUE(executor.execute(parser.parse("BEGIN;")).success);
        ASSERT_TRUE(executor
                        .execute(parser.parse(
                            "INSERT INTO Employees VALUES (1, \"Alice\", 120000.0), "
                            "(2, \"Bob\", 90000.0);"))
                        .success);
        ASSERT_TRUE(
            executor.execute(parser.parse("UPDATE Employees SET salary = 150000.0 WHERE id = 1;"))
                .success);
        ASSERT_TRUE(executor.execute(parser.parse("DELETE FROM Employees WHERE id = 2;")).success);

        EXPECT_EQ(WriteAheadLog{root / "VertexDB.wal"}.readAll().size(), 2U);

        ASSERT_TRUE(executor.execute(parser.parse("COMMIT;")).success);
    }

    const auto records = WriteAheadLog{root / "VertexDB.wal"}.readAll();
    ASSERT_EQ(records.size(), 3U);
    EXPECT_EQ(records[0].operation, WalOperation::CreateDatabase);
    EXPECT_EQ(records[1].operation, WalOperation::CreateTable);
    EXPECT_EQ(records[2].operation, WalOperation::PhysicalRedo);

    QueryExecutor recovered{root};
    auto result =
        recovered.execute(parser.parse("SELECT id, name, salary FROM Employees ORDER BY id;"));
    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.rows.size(), 1U);
    EXPECT_EQ(result.rows[0][0], Value{static_cast<std::int64_t>(1)});
    EXPECT_EQ(result.rows[0][1], Value{"Alice"});
    EXPECT_EQ(result.rows[0][2], Value{150000.0});
    std::filesystem::remove_all(root);
}

// --- Physical WAL redo and crash/partial-write recovery -------------------

TEST(DesiredBehaviorTests, PhysicalRedoRecoversInsertUpdateDeleteWithoutSqlPayload) {
    const auto root =
        std::filesystem::temp_directory_path() / "vertexdb-desired-physical-redo-recover";
    std::filesystem::remove_all(root);
    Parser parser;

    {
        QueryExecutor executor{root};
        ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
        ASSERT_TRUE(executor
                        .execute(parser.parse(
                            "CREATE TABLE Employees (id INT, name STRING, salary DOUBLE);"))
                        .success);
        ASSERT_TRUE(executor
                        .execute(parser.parse(
                            "INSERT INTO Employees VALUES (1, \"Alice\", 120000.0), "
                            "(2, \"Bob\", 90000.0);"))
                        .success);
        ASSERT_TRUE(
            executor.execute(parser.parse("UPDATE Employees SET salary = 150000.0 WHERE id = 1;"))
                .success);
        ASSERT_TRUE(executor.execute(parser.parse("DELETE FROM Employees WHERE id = 2;")).success);
    }

    const auto records = WriteAheadLog{root / "VertexDB.wal"}.readAll();
    ASSERT_GE(records.size(), 4U);
    std::size_t physicalCount = 0;
    for (const auto &record : records) {
        if (record.operation == WalOperation::PhysicalRedo) {
            ++physicalCount;
            // Physical payloads are binary row images, not SQL text.
            EXPECT_EQ(record.payload.find("INSERT"), std::string::npos);
            EXPECT_EQ(record.payload.find("UPDATE"), std::string::npos);
            EXPECT_EQ(record.payload.find("DELETE"), std::string::npos);
        } else if (record.operation == WalOperation::Insert ||
                   record.operation == WalOperation::Update ||
                   record.operation == WalOperation::Delete) {
            ADD_FAILURE() << "DML should use PhysicalRedo, not legacy logical SQL ops";
        }
    }
    EXPECT_EQ(physicalCount, 4U);

    QueryExecutor recovered{root};
    auto result =
        recovered.execute(parser.parse("SELECT id, name, salary FROM Employees ORDER BY id;"));
    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.rows.size(), 1U);
    EXPECT_EQ(result.rows[0][0], Value{static_cast<std::int64_t>(1)});
    EXPECT_EQ(result.rows[0][1], Value{"Alice"});
    EXPECT_EQ(result.rows[0][2], Value{150000.0});
    std::filesystem::remove_all(root);
}

TEST(DesiredBehaviorTests, TruncatedTrailingWalRecordIsIgnoredDuringRecovery) {
    const auto root =
        std::filesystem::temp_directory_path() / "vertexdb-desired-wal-partial-write";
    std::filesystem::remove_all(root);
    Parser parser;

    {
        QueryExecutor executor{root};
        ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
        ASSERT_TRUE(
            executor.execute(parser.parse("CREATE TABLE Events (id INT, label STRING);")).success);
        ASSERT_TRUE(
            executor.execute(parser.parse("INSERT INTO Events VALUES (1, \"ok\");")).success);
        ASSERT_TRUE(
            executor.execute(parser.parse("INSERT INTO Events VALUES (2, \"durable\");")).success);
    }

    const auto walPath = root / "VertexDB.wal";
    const auto complete = WriteAheadLog{walPath}.readAll();
    ASSERT_EQ(complete.size(), 4U);

    // Simulate a crash mid-append: keep complete records, then append a torn trailing fragment.
    {
        std::ofstream out{walPath, std::ios::binary | std::ios::app};
        ASSERT_TRUE(out);
        const char torn[] = {'T', 'C', 'W', 'A', '\x01', '\x00'}; // incomplete header
        out.write(torn, sizeof(torn));
        ASSERT_TRUE(out);
    }

    EXPECT_EQ(WriteAheadLog{walPath}.readAll().size(), complete.size());

    QueryExecutor recovered{root};
    auto result = recovered.execute(parser.parse("SELECT id, label FROM Events ORDER BY id;"));
    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.rows.size(), 2U);
    EXPECT_EQ(result.rows[0][0], Value{static_cast<std::int64_t>(1)});
    EXPECT_EQ(result.rows[0][1], Value{"ok"});
    EXPECT_EQ(result.rows[1][0], Value{static_cast<std::int64_t>(2)});
    EXPECT_EQ(result.rows[1][1], Value{"durable"});
    std::filesystem::remove_all(root);
}

TEST(DesiredBehaviorTests, TruncatedWalPayloadKeepsPriorPhysicalRedo) {
    const auto root =
        std::filesystem::temp_directory_path() / "vertexdb-desired-wal-torn-payload";
    std::filesystem::remove_all(root);
    Parser parser;

    {
        QueryExecutor executor{root};
        ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
        ASSERT_TRUE(executor.execute(parser.parse("CREATE TABLE Events (id INT);")).success);
        ASSERT_TRUE(executor.execute(parser.parse("INSERT INTO Events VALUES (1);")).success);
    }

    const auto walPath = root / "VertexDB.wal";
    ASSERT_EQ(WriteAheadLog{walPath}.readAll().size(), 3U);

    // Append a record header that claims a large payload, then write only a few bytes (torn body).
    {
        std::ofstream out{walPath, std::ios::binary | std::ios::app};
        ASSERT_TRUE(out);
        const std::uint32_t magic = 0x54435741;
        const std::uint32_t version = 1;
        const std::uint64_t lsn = 99;
        const std::uint8_t op = static_cast<std::uint8_t>(WalOperation::PhysicalRedo);
        const std::uint64_t claimedPayload = 1024;
        out.write(reinterpret_cast<const char *>(&magic), sizeof(magic));
        out.write(reinterpret_cast<const char *>(&version), sizeof(version));
        out.write(reinterpret_cast<const char *>(&lsn), sizeof(lsn));
        out.write(reinterpret_cast<const char *>(&op), sizeof(op));
        out.write(reinterpret_cast<const char *>(&claimedPayload), sizeof(claimedPayload));
        const char fragment[] = {'x', 'y', 'z'};
        out.write(fragment, sizeof(fragment));
        ASSERT_TRUE(out);
    }

    EXPECT_EQ(WriteAheadLog{walPath}.readAll().size(), 3U);

    QueryExecutor recovered{root};
    auto result = recovered.execute(parser.parse("SELECT id FROM Events;"));
    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.rows.size(), 1U);
    EXPECT_EQ(result.rows[0][0], Value{static_cast<std::int64_t>(1)});
    std::filesystem::remove_all(root);
}

TEST(DesiredBehaviorTests, TornTransactionBatchDoesNotPartiallyApply) {
    const auto root =
        std::filesystem::temp_directory_path() / "vertexdb-desired-wal-torn-txn-batch";
    std::filesystem::remove_all(root);
    Parser parser;

    {
        QueryExecutor executor{root};
        ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
        ASSERT_TRUE(executor.execute(parser.parse("CREATE TABLE Events (id INT);")).success);
        ASSERT_TRUE(executor.execute(parser.parse("INSERT INTO Events VALUES (1);")).success);
        ASSERT_TRUE(executor.execute(parser.parse("BEGIN;")).success);
        ASSERT_TRUE(executor.execute(parser.parse("INSERT INTO Events VALUES (2);")).success);
        ASSERT_TRUE(executor.execute(parser.parse("INSERT INTO Events VALUES (3);")).success);
        ASSERT_TRUE(executor.execute(parser.parse("COMMIT;")).success);
    }

    const auto walPath = root / "VertexDB.wal";
    auto records = WriteAheadLog{walPath}.readAll();
    ASSERT_EQ(records.size(), 4U);
    EXPECT_EQ(records[3].operation, WalOperation::PhysicalRedo);
    ASSERT_GT(records[3].payload.size(), 8U);

    // Truncate the durable transaction batch mid-payload (crash during COMMIT flush).
    {
        std::ifstream in{walPath, std::ios::binary};
        ASSERT_TRUE(in);
        std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        // Drop half of the final record's payload bytes while keeping prior records intact.
        const auto keep = bytes.size() - (records[3].payload.size() / 2);
        ASSERT_LT(keep, bytes.size());
        std::ofstream out{walPath, std::ios::binary | std::ios::trunc};
        ASSERT_TRUE(out);
        out.write(bytes.data(), static_cast<std::streamsize>(keep));
    }

    EXPECT_EQ(WriteAheadLog{walPath}.readAll().size(), 3U);

    QueryExecutor recovered{root};
    auto result = recovered.execute(parser.parse("SELECT id FROM Events ORDER BY id;"));
    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.rows.size(), 1U);
    EXPECT_EQ(result.rows[0][0], Value{static_cast<std::int64_t>(1)});
    std::filesystem::remove_all(root);
}

TEST(DesiredBehaviorTests, LegacyLogicalInsertWalStillReplays) {
    const auto root =
        std::filesystem::temp_directory_path() / "vertexdb-desired-legacy-logical-wal";
    std::filesystem::remove_all(root);
    Parser parser;

    WriteAheadLog wal{root / "VertexDB.wal"};
    wal.reset();
    (void)wal.append(WalOperation::CreateDatabase, "company");
    (void)wal.append(WalOperation::CreateTable,
                     "CREATE TABLE Employees (id INT, name STRING);");
    (void)wal.append(WalOperation::Insert, "INSERT INTO Employees VALUES (7, \"Legacy\");");

    QueryExecutor recovered{root};
    auto result = recovered.execute(parser.parse("SELECT id, name FROM Employees;"));
    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.rows.size(), 1U);
    EXPECT_EQ(result.rows[0][0], Value{static_cast<std::int64_t>(7)});
    EXPECT_EQ(result.rows[0][1], Value{"Legacy"});
    std::filesystem::remove_all(root);
}

TEST(DesiredBehaviorTests, StatsDrivenPlannerPrefersSelectiveEqualityOverLowCardinality) {
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
    Select query{"Employees", std::nullopt, {"*"}, where, {}, {}};
    const auto plan = QueryPlanner{}.planSelect(query, table);
    EXPECT_EQ(plan.accessPath, AccessPath::HashIndexLookup);
    EXPECT_EQ(plan.indexColumn, "id");
    EXPECT_LT(plan.estimatedCost, 2.0);
    ASSERT_TRUE(plan.residual.has_value());
    EXPECT_EQ(plan.residual->column, "dept");
}

TEST(DesiredBehaviorTests, StatsDrivenInLookupCostsScaleWithDistinctKeys) {
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
    Select query{"Employees", std::nullopt, {"*"}, where, {}, {}};
    const auto plan = QueryPlanner{}.planSelect(query, table);
    EXPECT_EQ(plan.accessPath, AccessPath::HashIndexLookup);
    EXPECT_EQ(plan.indexColumn, "id");
    EXPECT_LT(plan.estimatedCost, 3.0);
}

TEST(DesiredBehaviorTests, StatsDrivenJoinChoosesNestedLoopIndexProbe) {
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

TEST(DesiredBehaviorTests, StatsDrivenJoinKeepsHashWhenNeitherSideIndexed) {
    Table left{"Employees", {{"id", ColumnType::Int}, {"dept_id", ColumnType::Int}}};
    Table right{"Departments", {{"id", ColumnType::Int}, {"dept", ColumnType::String}}};
    left.insert({Value{1}, Value{10}});
    right.insert({Value{10}, Value{"Eng"}});

    JoinClause join{"Departments", "dept_id", "id"};
    const auto plan = QueryPlanner{}.planJoin(left, right, join);
    EXPECT_EQ(plan.algorithm, JoinAlgorithm::HashJoin);
    EXPECT_NE(plan.explanation.find("hash join"), std::string::npos);
}

TEST(DesiredBehaviorTests, IncrementalBTreeSplitMergeMaintainsStructuralInvariants) {
    constexpr std::size_t fanout = 2;
    BTreeIndex index{fanout};

    auto assertInvariants = [&](const BTreeIndex &tree) -> bool {
        const auto nodes = tree.nodesSnapshot();
        if (nodes.empty()) {
            ADD_FAILURE() << "empty node snapshot";
            return false;
        }

        std::unordered_map<BTreePageId, const BTreeNode *> byId;
        std::vector<const BTreeNode *> leaves;
        const BTreeNode *root = nullptr;
        for (const auto &node : nodes) {
            byId.emplace(node.pageId, &node);
            if (node.leaf) {
                leaves.push_back(&node);
            } else {
                root = &node; // nodesSnapshot keeps the root last among internals
            }
        }
        if (nodes.size() == 1) {
            root = &nodes.front();
        } else if (root == nullptr || root->leaf) {
            ADD_FAILURE() << "missing internal root";
            return false;
        }

        std::size_t linkedLeaves = 0;
        if (!leaves.empty()) {
            const BTreeNode *current = leaves.front();
            std::size_t leafIndex = 0;
            while (true) {
                if (leafIndex >= leaves.size() || current->pageId != leaves[leafIndex]->pageId) {
                    ADD_FAILURE() << "leaf chain diverged from snapshot order";
                    return false;
                }
                EXPECT_LE(current->keys.size(), fanout);
                EXPECT_EQ(current->keys.size(), current->rowIds.size());
                for (std::size_t i = 1; i < current->keys.size(); ++i) {
                    EXPECT_LT(current->keys[i - 1], current->keys[i]);
                }
                ++linkedLeaves;
                if (!current->nextLeaf) {
                    break;
                }
                auto it = byId.find(*current->nextLeaf);
                if (it == byId.end() || !it->second->leaf) {
                    ADD_FAILURE() << "broken nextLeaf link";
                    return false;
                }
                current = it->second;
                ++leafIndex;
            }
            EXPECT_EQ(linkedLeaves, leaves.size());
            EXPECT_EQ(linkedLeaves, tree.leafPageCount());
        }

        for (const auto &node : nodes) {
            if (node.leaf) {
                continue;
            }
            EXPECT_EQ(node.children.size(), node.keys.size() + 1U);
            EXPECT_LE(node.keys.size(), fanout);
            for (std::size_t i = 0; i < node.keys.size(); ++i) {
                auto childIt = byId.find(node.children[i + 1]);
                if (childIt == byId.end()) {
                    ADD_FAILURE() << "missing child page";
                    return false;
                }
                const auto &child = *childIt->second;
                if (child.leaf) {
                    if (child.keys.empty()) {
                        ADD_FAILURE() << "empty leaf under separator";
                        return false;
                    }
                    EXPECT_EQ(node.keys[i], child.keys.front());
                } else {
                    BTreePageId pageId = child.pageId;
                    while (!byId.at(pageId)->leaf) {
                        pageId = byId.at(pageId)->children.front();
                    }
                    if (byId.at(pageId)->keys.empty()) {
                        ADD_FAILURE() << "empty leftmost leaf under internal child";
                        return false;
                    }
                    EXPECT_EQ(node.keys[i], byId.at(pageId)->keys.front());
                }
            }
        }

        std::vector<Value> allKeys;
        if (!leaves.empty()) {
            const BTreeNode *current = leaves.front();
            while (true) {
                allKeys.insert(allKeys.end(), current->keys.begin(), current->keys.end());
                if (!current->nextLeaf) {
                    break;
                }
                current = byId.at(*current->nextLeaf);
            }
        }
        EXPECT_EQ(allKeys.size(), tree.size());
        for (std::size_t i = 1; i < allKeys.size(); ++i) {
            EXPECT_LT(allKeys[i - 1], allKeys[i]);
        }
        return true;
    };

    for (int key = 1; key <= 24; ++key) {
        index.insert(Value{key}, static_cast<RowId>(key * 10));
        ASSERT_TRUE(assertInvariants(index));
        EXPECT_EQ(index.find(Value{key}), std::vector<RowId>{static_cast<RowId>(key * 10)});
    }
    EXPECT_GE(index.height(), 2U);
    EXPECT_EQ(index.lessThan(Value{4}), (std::vector<RowId>{10, 20, 30}));
    EXPECT_EQ(index.greaterThan(Value{22}), (std::vector<RowId>{230, 240}));

    for (int key = 1; key <= 20; ++key) {
        index.remove(Value{key}, static_cast<RowId>(key * 10));
        ASSERT_TRUE(assertInvariants(index));
    }
    EXPECT_EQ(index.size(), 4U);
    EXPECT_EQ(index.find(Value{21}), std::vector<RowId>{210});

    for (int key = 21; key <= 24; ++key) {
        index.remove(Value{key}, static_cast<RowId>(key * 10));
        ASSERT_TRUE(assertInvariants(index));
    }
    EXPECT_EQ(index.size(), 0U);
    EXPECT_EQ(index.height(), 1U);
    EXPECT_EQ(index.leafPageCount(), 1U);
}

} // namespace VertexDB
