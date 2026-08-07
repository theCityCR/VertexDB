#include "test_support.hpp"

#include "VertexDB/execution/query_executor.hpp"
#include "VertexDB/execution/sql_literal.hpp"
#include "VertexDB/execution/txn_session.hpp"
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
    return makeTempExecutor("vertexdb-transaction-", suffix);
}

} // namespace

TEST(TransactionBehaviorTests, CommitPersistsTransactionMutations) {
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

TEST(TransactionBehaviorTests, RollbackKeepsSameDatabaseInstance) {
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

TEST(TransactionBehaviorTests, RollbackReversesMixedDmlAndIndexedLookups) {
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

TEST(TransactionBehaviorTests, SchemaChangesRejectedWhileTransactionActive) {
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

TEST(TransactionBehaviorTests, TxnSessionOwnsSnapshotUndoAndDeferredWalState) {
    TxnSession session;

    ASSERT_TRUE(session.begin().success);
    EXPECT_TRUE(session.transactionActive());
    EXPECT_EQ(session.readSnapshot().self, session.writeTransactionId());

    session.pushUndo(UndoRecord{"Employees", UndoKind::Insert, 7, std::nullopt});
    session.pushPendingWal(PendingWalRecord{WalOperation::Insert, "legacy insert"});
    EXPECT_EQ(session.undoLog().size(), 1U);
    EXPECT_EQ(session.pendingWal().size(), 1U);
    EXPECT_FALSE(session.rejectIfTransactionActive("CREATE TABLE").success);

    ASSERT_TRUE(session.rollback().success);
    EXPECT_FALSE(session.transactionActive());
    EXPECT_TRUE(session.undoLog().empty());
    EXPECT_TRUE(session.pendingWal().empty());
}

// --- P1 -----------------------------------------------------------------

TEST(TransactionBehaviorTests, UncommittedWritesInvisibleUntilCommit) {
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

TEST(TransactionBehaviorTests, SnapshotIsolationHidesCommitsAfterBegin) {
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

TEST(TransactionBehaviorTests, TransactionReadsOwnUncommittedWrites) {
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

TEST(TransactionBehaviorTests, RollbackDropsDeferredWalRecords) {
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

TEST(TransactionBehaviorTests, CommitFlushesDeferredWalAndRecovers) {
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
    EXPECT_EQ(records[2].operation, WalOperation::PageImageRedo);

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

} // namespace VertexDB
