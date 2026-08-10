#include "test_support.hpp"

#include "VertexDB/concurrency/lock_manager.hpp"
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
#include "VertexDB/transaction/transaction_manager.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <variant>
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

TEST(TransactionBehaviorTests, UpdateThenDeleteDoesNotResurrectPriorVersion) {
    // Regression: UPDATE must close the prior MVCC version so a later DELETE cannot expose it.
    Parser parser;
    auto executor = makeExecutor("update-then-delete");
    seedEmployees(executor, parser, true, false);

    ASSERT_TRUE(
        executor.execute(parser.parse("UPDATE Employees SET name = \"Alicia\" WHERE id = 1;"))
            .success);
    ASSERT_TRUE(executor.execute(parser.parse("DELETE FROM Employees WHERE id = 1;")).success);

    auto gone = executor.execute(parser.parse("SELECT name FROM Employees WHERE id = 1;"));
    ASSERT_TRUE(gone.success);
    EXPECT_TRUE(gone.rows.empty());

    auto remaining =
        executor.execute(parser.parse("SELECT id, name FROM Employees ORDER BY id ASC;"));
    ASSERT_TRUE(remaining.success);
    ASSERT_EQ(remaining.rows.size(), 2U);
    EXPECT_EQ(remaining.rows[0][0], Value{static_cast<std::int64_t>(2)});
    EXPECT_EQ(remaining.rows[1][0], Value{static_cast<std::int64_t>(3)});
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

TEST(TransactionBehaviorTests, CatalogAndPersistenceOpsRejectedWhileTransactionActive) {
    // Documented: CREATE DATABASE/TABLE, DROP/RENAME TABLE, SAVE/LOAD rejected in a txn.
    // CREATE INDEX is allowed (see CreateIndexAllowedWhileTransactionActive).
    Parser parser;
    auto executor = makeExecutor("txn-ddl-all");
    seedEmployees(executor, parser, true, false);
    ASSERT_TRUE(executor.execute(parser.parse("SAVE DATABASE;")).success);

    ASSERT_TRUE(executor.execute(parser.parse("BEGIN;")).success);

    const char *forbidden[] = {
        "CREATE DATABASE other;",
        "CREATE TABLE Other (id INT);",
        "DROP TABLE Employees;",
        "RENAME TABLE Employees TO Staff;",
        "SAVE DATABASE;",
        "LOAD DATABASE company;",
    };
    for (const char *sql : forbidden) {
        auto result = executor.execute(parser.parse(sql));
        EXPECT_FALSE(result.success) << sql << " -> " << result.message;
        EXPECT_NE(result.message.find("not allowed while a transaction is active"),
                  std::string::npos)
            << sql << " -> " << result.message;
    }

    ASSERT_TRUE(executor.execute(parser.parse("ROLLBACK;")).success);
    EXPECT_TRUE(executor.execute(parser.parse("LIST TABLES;")).success);
}

TEST(TransactionBehaviorTests, SchemaChangesRejectedWhileTransactionActive) {
    Parser parser;
    auto executor = makeExecutor("undo-ddl");
    seedEmployees(executor, parser, true, false);

    ASSERT_TRUE(executor.execute(parser.parse("BEGIN;")).success);
    auto create = executor.execute(parser.parse("CREATE TABLE Other (id INT);"));
    EXPECT_FALSE(create.success);
    EXPECT_NE(create.message.find("not allowed while a transaction is active"), std::string::npos);

    ASSERT_TRUE(executor.execute(parser.parse("ROLLBACK;")).success);
    auto after = executor.execute(parser.parse("CREATE TABLE Other (id INT);"));
    EXPECT_TRUE(after.success);
}

TEST(TransactionBehaviorTests, CreateIndexAllowedWhileTransactionActive) {
    Parser parser;
    auto executor = makeExecutor("txn-create-index");
    seedEmployees(executor, parser, false, false);

    ASSERT_TRUE(executor.execute(parser.parse("BEGIN;")).success);
    ASSERT_TRUE(
        executor.execute(parser.parse("CREATE INDEX idx_name ON Employees(name);")).success);
    auto explain =
        executor.execute(parser.parse("EXPLAIN SELECT id FROM Employees WHERE name = \"Alice\";"));
    ASSERT_TRUE(explain.success);
    ASSERT_FALSE(explain.rows.empty());
    EXPECT_NE(explain.rows.front().front().toString().find("hash index"), std::string::npos);
    ASSERT_TRUE(executor.execute(parser.parse("COMMIT;")).success);

    auto after =
        executor.execute(parser.parse("EXPLAIN SELECT id FROM Employees WHERE name = \"Alice\";"));
    ASSERT_TRUE(after.success);
    EXPECT_NE(after.rows.front().front().toString().find("hash index"), std::string::npos);
}

TEST(TransactionBehaviorTests, CreateIndexRollbackRemovesIndex) {
    Parser parser;
    auto executor = makeExecutor("txn-create-index-rb");
    seedEmployees(executor, parser, false, false);

    ASSERT_TRUE(executor.execute(parser.parse("BEGIN;")).success);
    ASSERT_TRUE(
        executor.execute(parser.parse("CREATE INDEX idx_name ON Employees(name);")).success);
    ASSERT_TRUE(executor.execute(parser.parse("ROLLBACK;")).success);

    auto explain =
        executor.execute(parser.parse("EXPLAIN SELECT id FROM Employees WHERE name = \"Alice\";"));
    ASSERT_TRUE(explain.success);
    ASSERT_FALSE(explain.rows.empty());
    EXPECT_NE(explain.rows.front().front().toString().find("full table scan"), std::string::npos);
}

TEST(TransactionBehaviorTests, CreateIndexWithInsertRollbackRestoresBoth) {
    Parser parser;
    auto executor = makeExecutor("txn-index-insert-rb");
    seedEmployees(executor, parser, false, false);

    ASSERT_TRUE(executor.execute(parser.parse("BEGIN;")).success);
    ASSERT_TRUE(
        executor.execute(parser.parse("CREATE INDEX idx_id ON Employees(id);")).success);
    ASSERT_TRUE(
        executor.execute(parser.parse("INSERT INTO Employees VALUES (99, \"Zed\", 1.0);")).success);
    ASSERT_TRUE(executor.execute(parser.parse("ROLLBACK;")).success);

    auto zed = executor.execute(parser.parse("SELECT id FROM Employees WHERE id = 99;"));
    ASSERT_TRUE(zed.success);
    EXPECT_TRUE(zed.rows.empty());

    auto explain =
        executor.execute(parser.parse("EXPLAIN SELECT name FROM Employees WHERE id = 1;"));
    ASSERT_TRUE(explain.success);
    EXPECT_NE(explain.rows.front().front().toString().find("full table scan"), std::string::npos);
}

TEST(TransactionBehaviorTests, CreateIndexCommitFlushesWalAndRecovers) {
    const auto root =
        std::filesystem::temp_directory_path() / "vertexdb-desired-wal-create-index";
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
                            "INSERT INTO Employees VALUES (1, \"Alice\", 120000.0);"))
                        .success);

        ASSERT_TRUE(executor.execute(parser.parse("BEGIN;")).success);
        ASSERT_TRUE(
            executor.execute(parser.parse("CREATE INDEX idx_id ON Employees(id);")).success);
        ASSERT_TRUE(executor.execute(parser.parse("COMMIT;")).success);
    }

    QueryExecutor recovered{root};
    auto explain =
        recovered.execute(parser.parse("EXPLAIN SELECT name FROM Employees WHERE id = 1;"));
    ASSERT_TRUE(explain.success);
    ASSERT_FALSE(explain.rows.empty());
    EXPECT_NE(explain.rows.front().front().toString().find("hash index"), std::string::npos);
    std::filesystem::remove_all(root);
}

TEST(TransactionBehaviorTests, BeginCommitRollbackRejectInvalidTxnState) {
    Parser parser;
    auto executor = makeExecutor("txn-state");
    seedEmployees(executor, parser, true, false);

    auto commitNone = executor.execute(parser.parse("COMMIT;"));
    EXPECT_FALSE(commitNone.success);
    EXPECT_NE(commitNone.message.find("no active transaction"), std::string::npos);

    auto rollbackNone = executor.execute(parser.parse("ROLLBACK;"));
    EXPECT_FALSE(rollbackNone.success);
    EXPECT_NE(rollbackNone.message.find("no active transaction"), std::string::npos);

    ASSERT_TRUE(executor.execute(parser.parse("BEGIN;")).success);
    auto nestedBegin = executor.execute(parser.parse("BEGIN;"));
    EXPECT_FALSE(nestedBegin.success);
    EXPECT_NE(nestedBegin.message.find("transaction already active"), std::string::npos);

    ASSERT_TRUE(executor.execute(parser.parse("ROLLBACK;")).success);
    EXPECT_FALSE(executor.execute(parser.parse("COMMIT;")).success);
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

// --- SI anomaly wedge -------------------------------------------------------
// Multi-txn interleaving uses shared Table + TransactionManager (one QueryExecutor
// cannot hold two open SQL transactions). Executor honesty tests cover the RW gate.

TEST(TransactionBehaviorTests, DirtyReadOfUncommittedUpdateIsPrevented) {
    // Desired: uncommitted UPDATE is invisible to another snapshot (no dirty read).
    TransactionManager transactions;
    Table table{"Employees",
                {{"id", ColumnType::Int}, {"name", ColumnType::String}, {"salary", ColumnType::Double}}};

    const auto aliceId =
        table.insert({Value{static_cast<std::int64_t>(1)}, Value{"Alice"}, Value{120000.0}},
                     transactions.beginCommitted());

    const auto reader = transactions.begin();
    const auto snap = transactions.currentSnapshot(reader.id);
    auto before = table.rowsSnapshot(snap, transactions);
    ASSERT_EQ(before.size(), 1U);
    EXPECT_EQ(before.front()[2], Value{120000.0});

    const auto writer = transactions.begin();
    ASSERT_TRUE(table.update(aliceId, 2, Value{999999.0}, writer.id));

    auto dirty = table.rowsSnapshot(snap, transactions);
    ASSERT_EQ(dirty.size(), 1U);
    EXPECT_EQ(dirty.front()[2], Value{120000.0});

    transactions.commit(writer.id);
    auto afterCommit = table.rowsSnapshot(transactions.currentSnapshot(), transactions);
    ASSERT_EQ(afterCommit.size(), 1U);
    EXPECT_EQ(afterCommit.front()[2], Value{999999.0});
}

TEST(TransactionBehaviorTests, SnapshotIsolationHidesCommittedInsertMatchingPredicate) {
    // Desired SI contract for a held snapshot: a row inserted+committed after BEGIN that
    // matches a predicate never appears in repeated reads of that snapshot (no mid-txn
    // phantom). SI does not abort the inserter (no SSI / predicate locks) — see write skew.
    TransactionManager transactions;
    Table table{"Employees",
                {{"id", ColumnType::Int}, {"name", ColumnType::String}, {"salary", ColumnType::Double}}};

    table.insert({Value{static_cast<std::int64_t>(1)}, Value{"Alice"}, Value{120000.0}},
                 transactions.beginCommitted());

    const auto reader = transactions.begin();
    const auto snap = transactions.currentSnapshot(reader.id);
    auto countHighSalary = [&](const ReadSnapshot &s) {
        std::size_t n = 0;
        for (const auto &row : table.rowsSnapshot(s, transactions)) {
            if (std::get<double>(row[2].data()) > 100000.0) {
                ++n;
            }
        }
        return n;
    };
    EXPECT_EQ(countHighSalary(snap), 1U);

    const auto concurrent = transactions.begin();
    table.insert({Value{static_cast<std::int64_t>(2)}, Value{"Bob"}, Value{110000.0}},
                 concurrent.id);
    transactions.commit(concurrent.id);

    EXPECT_EQ(countHighSalary(snap), 1U) << "held snapshot must not see the committed insert";
    EXPECT_EQ(countHighSalary(transactions.currentSnapshot()), 2U)
        << "a fresh snapshot may see the new row";
}

TEST(TransactionBehaviorTests, SnapshotIsolationAllowsWriteSkew) {
    // Documented SI limitation (not a bug): two txns each observe "at least one on-call"
    // then flip different rows off; both commit and the invariant is broken. SSI would abort.
    TransactionManager transactions;
    Table table{"Doctors", {{"id", ColumnType::Int}, {"on_call", ColumnType::Int}}};

    const auto docA =
        table.insert({Value{static_cast<std::int64_t>(1)}, Value{static_cast<std::int64_t>(1)}},
                     transactions.beginCommitted());
    const auto docB =
        table.insert({Value{static_cast<std::int64_t>(2)}, Value{static_cast<std::int64_t>(1)}},
                     transactions.beginCommitted());

    auto countOnCall = [&](const ReadSnapshot &s) {
        std::size_t n = 0;
        for (const auto &row : table.rowsSnapshot(s, transactions)) {
            if (std::get<std::int64_t>(row[1].data()) == 1) {
                ++n;
            }
        }
        return n;
    };

    const auto t1 = transactions.begin();
    const auto snap1 = transactions.currentSnapshot(t1.id);
    const auto t2 = transactions.begin();
    const auto snap2 = transactions.currentSnapshot(t2.id);
    ASSERT_EQ(countOnCall(snap1), 2U);
    ASSERT_EQ(countOnCall(snap2), 2U);

    ASSERT_TRUE(table.update(docA, 1, Value{static_cast<std::int64_t>(0)}, t1.id));
    ASSERT_TRUE(table.update(docB, 1, Value{static_cast<std::int64_t>(0)}, t2.id));
    transactions.commit(t1.id);
    transactions.commit(t2.id);

    EXPECT_EQ(countOnCall(transactions.currentSnapshot()), 0U)
        << "SI allows write skew; both commits succeed with zero on-call";
}

TEST(TransactionBehaviorTests, ExecutorAllowsConcurrentReadersAndWriterExcludesReaders) {
    // Honest sync model: shared SELECTs may overlap; a writer excludes readers on one executor.
    Parser parser;
    auto executor = makeExecutor("executor-rw-gate");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(
        executor.execute(parser.parse("CREATE TABLE Employees (id INT, name STRING);")).success);
    ASSERT_TRUE(executor.execute(parser.parse("INSERT INTO Employees VALUES (1, \"Alice\");"))
                    .success);

    // Parse once — Parser is not safe to share across threads.
    const auto selectQuery = parser.parse("SELECT name FROM Employees WHERE id = 1;");

    std::atomic<int> readersDone{0};
    std::atomic<bool> sawFailure{false};

    auto readerFn = [&] {
        auto result = executor.execute(selectQuery);
        if (!result.success || result.rows.size() != 1U) {
            sawFailure.store(true);
        }
        readersDone.fetch_add(1);
    };

    std::thread r1{readerFn};
    std::thread r2{readerFn};
    r1.join();
    r2.join();
    EXPECT_EQ(readersDone.load(), 2);
    EXPECT_FALSE(sawFailure.load());

    // LockManager-level: held read blocks write (executor uses the same pattern).
    LockManager locks;
    auto readLock = locks.acquireRead();
    auto writer = std::async(std::launch::async, [&locks] {
        const auto writeLock = locks.acquireWrite();
        return true;
    });
    EXPECT_EQ(writer.wait_for(std::chrono::milliseconds{25}), std::future_status::timeout);
    readLock.unlock();
    EXPECT_EQ(writer.wait_for(std::chrono::seconds{1}), std::future_status::ready);
    EXPECT_TRUE(writer.get());
}

} // namespace VertexDB
