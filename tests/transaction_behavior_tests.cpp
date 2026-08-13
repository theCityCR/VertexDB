#include "test_support.hpp"

#include "VertexDB/concurrency/lock_manager.hpp"
#include "VertexDB/execution/execution_context.hpp"
#include "VertexDB/execution/query_executor.hpp"
#include "VertexDB/execution/select_engine.hpp"
#include "VertexDB/execution/sql_literal.hpp"
#include "VertexDB/execution/subquery_runtime.hpp"
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

TEST(TransactionBehaviorTests, CatalogDdlAndSaveLoadAllowedWhileTransactionActive) {
    Parser parser;
    auto executor = makeExecutor("txn-ddl-all");
    seedEmployees(executor, parser, true, false);
    ASSERT_TRUE(executor.execute(parser.parse("SAVE DATABASE;")).success);

    ASSERT_TRUE(executor.execute(parser.parse("BEGIN;")).success);
    ASSERT_TRUE(executor.execute(parser.parse("CREATE TABLE Other (id INT);")).success);
    ASSERT_TRUE(executor.execute(parser.parse("DROP TABLE Other;")).success);
    ASSERT_TRUE(executor.execute(parser.parse("RENAME TABLE Employees TO Staff;")).success);
    // SAVE implicitly commits the open transaction, then checkpoints.
    auto saved = executor.execute(parser.parse("SAVE DATABASE;"));
    ASSERT_TRUE(saved.success);
    EXPECT_NE(saved.message.find("committed and saved"), std::string::npos);
    EXPECT_FALSE(executor.execute(parser.parse("COMMIT;")).success);

    ASSERT_TRUE(executor.execute(parser.parse("BEGIN;")).success);
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE other;")).success);
    ASSERT_TRUE(executor.execute(parser.parse("ROLLBACK;")).success);

    auto tables = executor.execute(parser.parse("LIST TABLES;"));
    ASSERT_TRUE(tables.success);
    ASSERT_EQ(tables.rows.size(), 1U);
    EXPECT_EQ(tables.rows[0][0], Value{"Staff"});
}

TEST(TransactionBehaviorTests, SaveDatabaseInTransactionCommitsThenCheckpoints) {
    const auto root =
        std::filesystem::temp_directory_path() / "vertexdb-desired-save-in-txn";
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
                            "INSERT INTO Employees VALUES (1, \"Alice\", 120000.0);"))
                        .success);
        auto saved = executor.execute(parser.parse("SAVE DATABASE;"));
        ASSERT_TRUE(saved.success);
        EXPECT_NE(saved.message.find("committed and saved"), std::string::npos);
    }

    QueryExecutor recovered{root};
    auto rows = recovered.execute(parser.parse("SELECT id FROM Employees;"));
    ASSERT_TRUE(rows.success);
    ASSERT_EQ(rows.rows.size(), 1U);
    EXPECT_EQ(rows.rows[0][0], Value{static_cast<std::int64_t>(1)});
    std::filesystem::remove_all(root);
}

TEST(TransactionBehaviorTests, LoadDatabaseInTransactionRollsBackThenLoads) {
    const auto root =
        std::filesystem::temp_directory_path() / "vertexdb-desired-load-in-txn";
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
        ASSERT_TRUE(executor.execute(parser.parse("SAVE DATABASE;")).success);

        ASSERT_TRUE(executor.execute(parser.parse("BEGIN;")).success);
        ASSERT_TRUE(executor
                        .execute(parser.parse(
                            "INSERT INTO Employees VALUES (2, \"Bob\", 90000.0);"))
                        .success);
        auto loaded = executor.execute(parser.parse("LOAD DATABASE company;"));
        ASSERT_TRUE(loaded.success);
        EXPECT_NE(loaded.message.find("rolled back and loaded"), std::string::npos);

        auto rows =
            executor.execute(parser.parse("SELECT id FROM Employees ORDER BY id;"));
        ASSERT_TRUE(rows.success);
        ASSERT_EQ(rows.rows.size(), 1U);
        EXPECT_EQ(rows.rows[0][0], Value{static_cast<std::int64_t>(1)});
        EXPECT_FALSE(executor.execute(parser.parse("ROLLBACK;")).success);
    }
    std::filesystem::remove_all(root);
}

TEST(TransactionBehaviorTests, CreateTableRollbackRemovesTable) {
    Parser parser;
    auto executor = makeExecutor("txn-create-table-rb");
    seedEmployees(executor, parser, false, false);

    ASSERT_TRUE(executor.execute(parser.parse("BEGIN;")).success);
    ASSERT_TRUE(executor.execute(parser.parse("CREATE TABLE Other (id INT);")).success);
    ASSERT_TRUE(executor.execute(parser.parse("INSERT INTO Other VALUES (1);")).success);
    ASSERT_TRUE(executor.execute(parser.parse("ROLLBACK;")).success);

    auto listed = executor.execute(parser.parse("LIST TABLES;"));
    ASSERT_TRUE(listed.success);
    ASSERT_EQ(listed.rows.size(), 1U);
    EXPECT_EQ(listed.rows[0][0], Value{"Employees"});
}

TEST(TransactionBehaviorTests, DropTableRollbackRestoresTableAndRows) {
    Parser parser;
    auto executor = makeExecutor("txn-drop-table-rb");
    seedEmployees(executor, parser, true, false);

    ASSERT_TRUE(executor.execute(parser.parse("BEGIN;")).success);
    ASSERT_TRUE(executor.execute(parser.parse("DROP TABLE Employees;")).success);
    EXPECT_THROW((void)executor.execute(parser.parse("SELECT id FROM Employees;")),
                 std::runtime_error);
    ASSERT_TRUE(executor.execute(parser.parse("ROLLBACK;")).success);

    auto rows = executor.execute(parser.parse("SELECT id FROM Employees ORDER BY id;"));
    ASSERT_TRUE(rows.success);
    EXPECT_EQ(rows.rows.size(), 3U);
    auto explain =
        executor.execute(parser.parse("EXPLAIN SELECT name FROM Employees WHERE id = 1;"));
    ASSERT_TRUE(explain.success);
    EXPECT_NE(explain.rows.front().front().toString().find("hash index"), std::string::npos);
}

TEST(TransactionBehaviorTests, RenameTableRollbackRestoresNameAndPendingDml) {
    Parser parser;
    auto executor = makeExecutor("txn-rename-rb");
    seedEmployees(executor, parser, true, false);

    ASSERT_TRUE(executor.execute(parser.parse("BEGIN;")).success);
    ASSERT_TRUE(
        executor.execute(parser.parse("UPDATE Employees SET name = \"Alicia\" WHERE id = 1;"))
            .success);
    ASSERT_TRUE(executor.execute(parser.parse("RENAME TABLE Employees TO Staff;")).success);
    auto renamed = executor.execute(parser.parse("SELECT name FROM Staff WHERE id = 1;"));
    ASSERT_TRUE(renamed.success);
    ASSERT_EQ(renamed.rows.size(), 1U);
    EXPECT_EQ(renamed.rows[0][0], Value{"Alicia"});
    ASSERT_TRUE(executor.execute(parser.parse("ROLLBACK;")).success);

    auto restored = executor.execute(parser.parse("SELECT name FROM Employees WHERE id = 1;"));
    ASSERT_TRUE(restored.success);
    ASSERT_EQ(restored.rows.size(), 1U);
    EXPECT_EQ(restored.rows[0][0], Value{"Alice"});
    EXPECT_THROW((void)executor.execute(parser.parse("SELECT name FROM Staff;")),
                 std::runtime_error);
}

TEST(TransactionBehaviorTests, CreateDatabaseRollbackRestoresPriorDatabase) {
    Parser parser;
    auto executor = makeExecutor("txn-create-db-rb");
    seedEmployees(executor, parser, false, false);
    const auto prior = executor.currentDatabase();
    ASSERT_NE(prior, nullptr);

    ASSERT_TRUE(executor.execute(parser.parse("BEGIN;")).success);
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE other;")).success);
    EXPECT_NE(executor.currentDatabase(), prior);
    ASSERT_TRUE(executor.execute(parser.parse("ROLLBACK;")).success);
    EXPECT_EQ(executor.currentDatabase(), prior);
    auto rows = executor.execute(parser.parse("SELECT id FROM Employees;"));
    ASSERT_TRUE(rows.success);
    EXPECT_EQ(rows.rows.size(), 3U);
}

TEST(TransactionBehaviorTests, CreateTableCommitFlushesWalAndRecovers) {
    const auto root =
        std::filesystem::temp_directory_path() / "vertexdb-desired-wal-create-table";
    std::filesystem::remove_all(root);
    Parser parser;

    {
        QueryExecutor executor{root};
        ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
        ASSERT_TRUE(executor.execute(parser.parse("BEGIN;")).success);
        ASSERT_TRUE(executor
                        .execute(parser.parse(
                            "CREATE TABLE Employees (id INT, name STRING, salary DOUBLE);"))
                        .success);
        ASSERT_TRUE(executor.execute(parser.parse("COMMIT;")).success);
    }

    QueryExecutor recovered{root};
    auto listed = recovered.execute(parser.parse("LIST TABLES;"));
    ASSERT_TRUE(listed.success);
    ASSERT_EQ(listed.rows.size(), 1U);
    EXPECT_EQ(listed.rows[0][0], Value{"Employees"});
    std::filesystem::remove_all(root);
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

TEST(TransactionBehaviorTests, DropIndexRemovesLookup) {
    Parser parser;
    auto executor = makeExecutor("drop-index");
    seedEmployees(executor, parser, true, false);

    auto before =
        executor.execute(parser.parse("EXPLAIN SELECT name FROM Employees WHERE id = 1;"));
    ASSERT_TRUE(before.success);
    EXPECT_NE(before.rows.front().front().toString().find("hash index"), std::string::npos);

    ASSERT_TRUE(executor.execute(parser.parse("DROP INDEX idx_id ON Employees;")).success);
    auto after =
        executor.execute(parser.parse("EXPLAIN SELECT name FROM Employees WHERE id = 1;"));
    ASSERT_TRUE(after.success);
    EXPECT_NE(after.rows.front().front().toString().find("full table scan"), std::string::npos);

    auto missing = executor.execute(parser.parse("DROP INDEX idx_id ON Employees;"));
    EXPECT_FALSE(missing.success);
    EXPECT_NE(missing.message.find("unknown index"), std::string::npos);
}

TEST(TransactionBehaviorTests, DropIndexRollbackRestoresIndex) {
    Parser parser;
    auto executor = makeExecutor("txn-drop-index-rb");
    seedEmployees(executor, parser, true, false);

    ASSERT_TRUE(executor.execute(parser.parse("BEGIN;")).success);
    ASSERT_TRUE(executor.execute(parser.parse("DROP INDEX idx_id ON Employees;")).success);
    auto mid =
        executor.execute(parser.parse("EXPLAIN SELECT name FROM Employees WHERE id = 1;"));
    ASSERT_TRUE(mid.success);
    EXPECT_NE(mid.rows.front().front().toString().find("full table scan"), std::string::npos);
    ASSERT_TRUE(executor.execute(parser.parse("ROLLBACK;")).success);

    auto after =
        executor.execute(parser.parse("EXPLAIN SELECT name FROM Employees WHERE id = 1;"));
    ASSERT_TRUE(after.success);
    EXPECT_NE(after.rows.front().front().toString().find("hash index"), std::string::npos);
}

TEST(TransactionBehaviorTests, DropIndexCommitFlushesWalAndRecovers) {
    const auto root =
        std::filesystem::temp_directory_path() / "vertexdb-desired-wal-drop-index";
    std::filesystem::remove_all(root);
    Parser parser;

    {
        QueryExecutor executor{root};
        ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
        ASSERT_TRUE(executor
                        .execute(parser.parse(
                            "CREATE TABLE Employees (id INT, name STRING, salary DOUBLE);"))
                        .success);
        ASSERT_TRUE(
            executor.execute(parser.parse("CREATE INDEX idx_id ON Employees(id);")).success);
        ASSERT_TRUE(executor
                        .execute(parser.parse(
                            "INSERT INTO Employees VALUES (1, \"Alice\", 120000.0);"))
                        .success);

        ASSERT_TRUE(executor.execute(parser.parse("BEGIN;")).success);
        ASSERT_TRUE(executor.execute(parser.parse("DROP INDEX idx_id ON Employees;")).success);
        ASSERT_TRUE(executor.execute(parser.parse("COMMIT;")).success);
    }

    QueryExecutor recovered{root};
    auto explain =
        recovered.execute(parser.parse("EXPLAIN SELECT name FROM Employees WHERE id = 1;"));
    ASSERT_TRUE(explain.success);
    ASSERT_FALSE(explain.rows.empty());
    EXPECT_NE(explain.rows.front().front().toString().find("full table scan"), std::string::npos);
    std::filesystem::remove_all(root);
}

// Phase 4a checklist: DROP TABLE commit must leave a durable catalog change after restart.
TEST(TransactionBehaviorTests, DropTableCommitFlushesWalAndRecovers) {
    const auto root =
        std::filesystem::temp_directory_path() / "vertexdb-desired-wal-drop-table";
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
                            "CREATE TABLE Other (id INT);"))
                        .success);
        ASSERT_TRUE(executor.execute(parser.parse("BEGIN;")).success);
        ASSERT_TRUE(executor.execute(parser.parse("DROP TABLE Other;")).success);
        ASSERT_TRUE(executor.execute(parser.parse("COMMIT;")).success);
    }

    QueryExecutor recovered{root};
    auto listed = recovered.execute(parser.parse("LIST TABLES;"));
    ASSERT_TRUE(listed.success);
    ASSERT_EQ(listed.rows.size(), 1U);
    EXPECT_EQ(listed.rows[0][0], Value{"Employees"});
    std::filesystem::remove_all(root);
}

// Phase 4a checklist: RENAME TABLE commit remounts under the new name after WAL recovery.
TEST(TransactionBehaviorTests, RenameTableCommitFlushesWalAndRecovers) {
    const auto root =
        std::filesystem::temp_directory_path() / "vertexdb-desired-wal-rename-table";
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
        ASSERT_TRUE(executor.execute(parser.parse("RENAME TABLE Employees TO Staff;")).success);
        ASSERT_TRUE(executor.execute(parser.parse("COMMIT;")).success);
    }

    QueryExecutor recovered{root};
    auto listed = recovered.execute(parser.parse("LIST TABLES;"));
    ASSERT_TRUE(listed.success);
    ASSERT_EQ(listed.rows.size(), 1U);
    EXPECT_EQ(listed.rows[0][0], Value{"Staff"});
    auto rows = recovered.execute(parser.parse("SELECT name FROM Staff WHERE id = 1;"));
    ASSERT_TRUE(rows.success);
    ASSERT_EQ(rows.rows.size(), 1U);
    EXPECT_EQ(rows.rows[0][0], Value{"Alice"});
    std::filesystem::remove_all(root);
}

TEST(TransactionBehaviorTests, AddNullableColumnAppearsAsNullOnSelect) {
    Parser parser;
    auto executor = makeExecutor("alter-add-null-select");
    seedEmployees(executor, parser, false, false);

    ASSERT_TRUE(
        executor.execute(parser.parse("ALTER TABLE Employees ADD COLUMN nickname STRING NULL;"))
            .success);
    auto rows =
        executor.execute(parser.parse("SELECT id, nickname FROM Employees WHERE id = 1;"));
    ASSERT_TRUE(rows.success);
    ASSERT_EQ(rows.rows.size(), 1U);
    EXPECT_EQ(rows.rows[0][0], Value{static_cast<std::int64_t>(1)});
    EXPECT_TRUE(rows.rows[0][1].isNull());
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "INSERT INTO Employees VALUES (4, \"Dee\", 50000.0, NULL);"))
                    .success);
    auto dee = executor.execute(parser.parse("SELECT nickname FROM Employees WHERE id = 4;"));
    ASSERT_TRUE(dee.success);
    ASSERT_EQ(dee.rows.size(), 1U);
    EXPECT_TRUE(dee.rows[0][0].isNull());
}

TEST(TransactionBehaviorTests, AddColumnRollbackRemovesColumnAndRestoresWidth) {
    Parser parser;
    auto executor = makeExecutor("alter-add-rb");
    seedEmployees(executor, parser, false, false);

    ASSERT_TRUE(executor.execute(parser.parse("BEGIN;")).success);
    ASSERT_TRUE(
        executor.execute(parser.parse("ALTER TABLE Employees ADD COLUMN nickname STRING NULL;"))
            .success);
    ASSERT_TRUE(executor.execute(parser.parse("ROLLBACK;")).success);

    EXPECT_THROW((void)executor.execute(parser.parse("SELECT nickname FROM Employees WHERE id = 1;")),
                 std::runtime_error);
    auto rows = executor.execute(parser.parse("SELECT id, name, salary FROM Employees WHERE id = 1;"));
    ASSERT_TRUE(rows.success);
    ASSERT_EQ(rows.rows.size(), 1U);
    EXPECT_EQ(rows.rows[0][0], Value{static_cast<std::int64_t>(1)});
}

TEST(TransactionBehaviorTests, DropColumnRollbackRestoresColumnAndValues) {
    Parser parser;
    auto executor = makeExecutor("alter-drop-rb");
    seedEmployees(executor, parser, false, false);
    ASSERT_TRUE(
        executor.execute(parser.parse("ALTER TABLE Employees ADD COLUMN nickname STRING NULL;"))
            .success);
    ASSERT_TRUE(executor
                    .execute(parser.parse(
                        "UPDATE Employees SET nickname = \"Ally\" WHERE id = 1;"))
                    .success);

    ASSERT_TRUE(executor.execute(parser.parse("BEGIN;")).success);
    ASSERT_TRUE(
        executor.execute(parser.parse("ALTER TABLE Employees DROP COLUMN nickname;")).success);
    EXPECT_THROW((void)executor.execute(parser.parse("SELECT nickname FROM Employees WHERE id = 1;")),
                 std::runtime_error);
    ASSERT_TRUE(executor.execute(parser.parse("ROLLBACK;")).success);

    auto rows =
        executor.execute(parser.parse("SELECT nickname FROM Employees WHERE id = 1;"));
    ASSERT_TRUE(rows.success);
    ASSERT_EQ(rows.rows.size(), 1U);
    EXPECT_EQ(rows.rows[0][0], Value{"Ally"});
}

TEST(TransactionBehaviorTests, AddColumnCommitFlushesWalAndRecovers) {
    const auto root =
        std::filesystem::temp_directory_path() / "vertexdb-desired-wal-alter-add";
    std::filesystem::remove_all(root);
    Parser parser;

    {
        QueryExecutor executor{root};
        ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
        ASSERT_TRUE(executor
                        .execute(parser.parse(
                            "CREATE TABLE Employees (id INT, name STRING);"))
                        .success);
        ASSERT_TRUE(
            executor.execute(parser.parse("INSERT INTO Employees VALUES (1, \"Alice\");")).success);
        ASSERT_TRUE(executor.execute(parser.parse("BEGIN;")).success);
        ASSERT_TRUE(
            executor
                .execute(parser.parse("ALTER TABLE Employees ADD COLUMN nickname STRING NULL;"))
                .success);
        ASSERT_TRUE(executor.execute(parser.parse("COMMIT;")).success);
    }

    QueryExecutor recovered{root};
    auto rows =
        recovered.execute(parser.parse("SELECT id, nickname FROM Employees WHERE id = 1;"));
    ASSERT_TRUE(rows.success);
    ASSERT_EQ(rows.rows.size(), 1U);
    EXPECT_EQ(rows.rows[0][0], Value{static_cast<std::int64_t>(1)});
    EXPECT_TRUE(rows.rows[0][1].isNull());
    std::filesystem::remove_all(root);
}

TEST(TransactionBehaviorTests, DropColumnCommitFlushesWalAndRecovers) {
    const auto root =
        std::filesystem::temp_directory_path() / "vertexdb-desired-wal-alter-drop";
    std::filesystem::remove_all(root);
    Parser parser;

    {
        QueryExecutor executor{root};
        ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
        ASSERT_TRUE(executor
                        .execute(parser.parse(
                            "CREATE TABLE Employees (id INT, name STRING, note STRING NULL);"))
                        .success);
        ASSERT_TRUE(executor
                        .execute(parser.parse(
                            "INSERT INTO Employees VALUES (1, \"Alice\", \"keep\");"))
                        .success);
        ASSERT_TRUE(executor.execute(parser.parse("BEGIN;")).success);
        ASSERT_TRUE(
            executor.execute(parser.parse("ALTER TABLE Employees DROP COLUMN note;")).success);
        ASSERT_TRUE(executor.execute(parser.parse("COMMIT;")).success);
    }

    QueryExecutor recovered{root};
    EXPECT_THROW((void)recovered.execute(parser.parse("SELECT note FROM Employees WHERE id = 1;")),
                 std::runtime_error);
    auto rows = recovered.execute(parser.parse("SELECT id, name FROM Employees WHERE id = 1;"));
    ASSERT_TRUE(rows.success);
    ASSERT_EQ(rows.rows.size(), 1U);
    EXPECT_EQ(rows.rows[0][1], Value{"Alice"});
    std::filesystem::remove_all(root);
}

TEST(TransactionBehaviorTests, SaveLoadPreservesAddedColumn) {
    Parser parser;
    auto executor = makeExecutor("alter-save-load");
    seedEmployees(executor, parser, false, false);
    ASSERT_TRUE(
        executor.execute(parser.parse("ALTER TABLE Employees ADD COLUMN nickname STRING NULL;"))
            .success);
    ASSERT_TRUE(executor.execute(parser.parse("SAVE DATABASE;")).success);
    ASSERT_TRUE(executor.execute(parser.parse("LOAD DATABASE company;")).success);
    auto rows =
        executor.execute(parser.parse("SELECT id, nickname FROM Employees WHERE id = 2;"));
    ASSERT_TRUE(rows.success);
    ASSERT_EQ(rows.rows.size(), 1U);
    EXPECT_TRUE(rows.rows[0][1].isNull());
}

// Phase 4a checklist: mixed catalog DDL + DML in one txn is one atomic commit batch.
TEST(TransactionBehaviorTests, CatalogAndDmlMixedCommitFlushesWalAndRecovers) {
    const auto root =
        std::filesystem::temp_directory_path() / "vertexdb-desired-wal-catalog-dml-mixed";
    std::filesystem::remove_all(root);
    Parser parser;

    {
        QueryExecutor executor{root};
        ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
        ASSERT_TRUE(executor.execute(parser.parse("BEGIN;")).success);
        ASSERT_TRUE(executor
                        .execute(parser.parse(
                            "CREATE TABLE Items (id INT, label STRING);"))
                        .success);
        ASSERT_TRUE(
            executor.execute(parser.parse("INSERT INTO Items VALUES (1, \"mixed\");")).success);
        ASSERT_TRUE(executor.execute(parser.parse("COMMIT;")).success);
    }

    QueryExecutor recovered{root};
    auto rows = recovered.execute(parser.parse("SELECT id, label FROM Items;"));
    ASSERT_TRUE(rows.success);
    ASSERT_EQ(rows.rows.size(), 1U);
    EXPECT_EQ(rows.rows[0][0], Value{static_cast<std::int64_t>(1)});
    EXPECT_EQ(rows.rows[0][1], Value{"mixed"});
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

// Desired: COMMIT returns only after deferred redo is on durable storage (flush+fsync via
// WriteAheadLog::append). A new process must recover the committed rows from the WAL alone.
TEST(TransactionBehaviorTests, CommitReturnsOnlyAfterDeferredWalIsDurable) {
    const auto root =
        std::filesystem::temp_directory_path() / "vertexdb-desired-wal-commit-durable";
    std::filesystem::remove_all(root);
    Parser parser;
    const auto walPath = root / "VertexDB.wal";

    {
        QueryExecutor executor{root};
        ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
        ASSERT_TRUE(
            executor.execute(parser.parse("CREATE TABLE Items (id INT, label STRING);")).success);

        ASSERT_TRUE(executor.execute(parser.parse("BEGIN;")).success);
        ASSERT_TRUE(
            executor.execute(parser.parse("INSERT INTO Items VALUES (1, \"durable\");")).success);
        // Still deferred: no DML redo on disk until COMMIT.
        ASSERT_EQ(WriteAheadLog{walPath}.readAll().size(), 2U);

        ASSERT_TRUE(executor.execute(parser.parse("COMMIT;")).success);

        // After a successful COMMIT, the WAL file must already contain the batch redo — not
        // merely buffered in the executor — so a crash here would still recover the insert.
        const auto records = WriteAheadLog{walPath}.readAll();
        ASSERT_EQ(records.size(), 3U);
        EXPECT_EQ(records[2].operation, WalOperation::PageImageRedo);
        EXPECT_GT(std::filesystem::file_size(walPath), 0U);
    }

    // Drop in-memory state; recovery must replay the durable COMMIT batch.
    QueryExecutor recovered{root};
    auto result = recovered.execute(parser.parse("SELECT id, label FROM Items;"));
    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.rows.size(), 1U);
    EXPECT_EQ(result.rows[0][0], Value{static_cast<std::int64_t>(1)});
    EXPECT_EQ(result.rows[0][1], Value{"durable"});
    std::filesystem::remove_all(root);
}

// Desired (ACID Phase 3c): if the process dies after durable WAL sync but before the in-memory
// commit mark, recovery must still replay the committed batch.
TEST(TransactionBehaviorTests, RecoverSurvivesCrashAfterWalSyncBeforeCommitMark) {
    const auto root =
        std::filesystem::temp_directory_path() / "vertexdb-desired-crash-after-wal-sync";
    std::filesystem::remove_all(root);
    Parser parser;
    const auto walPath = root / "VertexDB.wal";

    {
        QueryExecutor executor{root};
        ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
        ASSERT_TRUE(
            executor.execute(parser.parse("CREATE TABLE Items (id INT, label STRING);")).success);
        ASSERT_TRUE(executor.execute(parser.parse("BEGIN;")).success);
        ASSERT_TRUE(
            executor.execute(parser.parse("INSERT INTO Items VALUES (1, \"synced\");")).success);
        ASSERT_EQ(WriteAheadLog{walPath}.readAll().size(), 2U);

        executor.armCrashInjection(CrashInjectionPoint::AfterWalSyncBeforeCommitMark);
        EXPECT_THROW(executor.execute(parser.parse("COMMIT;")), CrashInjected);

        // WAL already holds the batch; only the in-memory commit mark was skipped.
        const auto records = WriteAheadLog{walPath}.readAll();
        ASSERT_EQ(records.size(), 3U);
        EXPECT_EQ(records[2].operation, WalOperation::PageImageRedo);
    }

    QueryExecutor recovered{root};
    auto result = recovered.execute(parser.parse("SELECT id, label FROM Items;"));
    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.rows.size(), 1U);
    EXPECT_EQ(result.rows[0][0], Value{static_cast<std::int64_t>(1)});
    EXPECT_EQ(result.rows[0][1], Value{"synced"});
    std::filesystem::remove_all(root);
}

// Desired complementary cut point: die before WAL sync — uncommitted DML must not survive restart.
TEST(TransactionBehaviorTests, CrashBeforeWalSyncDoesNotDurableCommit) {
    const auto root =
        std::filesystem::temp_directory_path() / "vertexdb-desired-crash-before-wal-sync";
    std::filesystem::remove_all(root);
    Parser parser;
    const auto walPath = root / "VertexDB.wal";

    {
        QueryExecutor executor{root};
        ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
        ASSERT_TRUE(
            executor.execute(parser.parse("CREATE TABLE Items (id INT, label STRING);")).success);
        ASSERT_TRUE(executor.execute(parser.parse("BEGIN;")).success);
        ASSERT_TRUE(
            executor.execute(parser.parse("INSERT INTO Items VALUES (1, \"lost\");")).success);

        executor.armCrashInjection(CrashInjectionPoint::BeforeWalSync);
        EXPECT_THROW(executor.execute(parser.parse("COMMIT;")), CrashInjected);
        EXPECT_EQ(WriteAheadLog{walPath}.readAll().size(), 2U)
            << "crash before WAL sync must not append the deferred DML batch";
    }

    QueryExecutor recovered{root};
    auto result = recovered.execute(parser.parse("SELECT id, label FROM Items;"));
    ASSERT_TRUE(result.success);
    EXPECT_TRUE(result.rows.empty())
        << "uncommitted insert must not be durable when COMMIT dies before WAL sync";
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
    // phantom). This Table::rowsSnapshot path records row reads only — insert-phantom SSI
    // requires an explicit predicate read (see SerializableSnapshotIsolationAbortsInsertPhantom).
    TransactionManager transactions;
    Table table{"Employees",
                {{"id", ColumnType::Int}, {"name", ColumnType::String}, {"salary", ColumnType::Double}}};

    table.insert({Value{static_cast<std::int64_t>(1)}, Value{"Alice"}, Value{120000.0}},
                 transactions.beginCommitted(), &transactions);

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
                 concurrent.id, &transactions);
    transactions.commit(concurrent.id);

    EXPECT_EQ(countHighSalary(snap), 1U) << "held snapshot must not see the committed insert";
    EXPECT_EQ(countHighSalary(transactions.currentSnapshot()), 2U)
        << "a fresh snapshot may see the new row";
    transactions.commit(reader.id);
}

TEST(TransactionBehaviorTests, SerializableSnapshotIsolationAbortsInsertPhantom) {
    // Classic insert phantom under SSI: T1 observes salary > 100000 (predicate SIREAD), T2
    // inserts a matching row. First committer wins; the later commit throws.
    TransactionManager transactions;
    Table table{"Employees",
                {{"id", ColumnType::Int}, {"name", ColumnType::String}, {"salary", ColumnType::Double}}};

    table.insert({Value{static_cast<std::int64_t>(1)}, Value{"Alice"}, Value{120000.0}},
                 transactions.beginCommitted(), &transactions);

    const auto t1 = transactions.begin();
    const auto snap1 = transactions.currentSnapshot(t1.id);
    transactions.recordPredicateRead(
        t1.id, SsiPredicate{"Employees", "salary", ComparisonOperator::Greater, Value{100000.0}});
    std::size_t high = 0;
    for (const auto &row : table.rowsSnapshot(snap1, transactions)) {
        if (std::get<double>(row[2].data()) > 100000.0) {
            ++high;
        }
    }
    ASSERT_EQ(high, 1U);

    const auto t2 = transactions.begin();
    table.insert({Value{static_cast<std::int64_t>(2)}, Value{"Bob"}, Value{110000.0}}, t2.id,
                 &transactions);
    transactions.commit(t2.id);
    EXPECT_THROW(transactions.commit(t1.id), SerializationFailure)
        << "SSI aborts the predicate reader after a concurrent matching insert commits";
}

TEST(TransactionBehaviorTests, SerializableSnapshotIsolationAbortsInsertPhantomEmptyProbe) {
    // Empty predicate observation still takes a SIREAD: T1 sees no rows with id = 99, T2
    // inserts that key; later committer aborts.
    TransactionManager transactions;
    Table table{"Employees",
                {{"id", ColumnType::Int}, {"name", ColumnType::String}, {"salary", ColumnType::Double}}};

    table.insert({Value{static_cast<std::int64_t>(1)}, Value{"Alice"}, Value{120000.0}},
                 transactions.beginCommitted(), &transactions);

    const auto t1 = transactions.begin();
    transactions.recordPredicateRead(
        t1.id, SsiPredicate{"Employees", "id", ComparisonOperator::Equal,
                            Value{static_cast<std::int64_t>(99)}});
    const auto snap1 = transactions.currentSnapshot(t1.id);
    std::size_t hits = 0;
    for (const auto &row : table.rowsSnapshot(snap1, transactions)) {
        if (std::get<std::int64_t>(row[0].data()) == 99) {
            ++hits;
        }
    }
    ASSERT_EQ(hits, 0U);

    const auto t2 = transactions.begin();
    table.insert({Value{static_cast<std::int64_t>(99)}, Value{"Zed"}, Value{50000.0}}, t2.id,
                 &transactions);
    // Reader commits first (read-only) — retains committed predicate reads for concurrent T2.
    transactions.commit(t1.id);
    EXPECT_THROW(transactions.commit(t2.id), SerializationFailure)
        << "SSI aborts the inserter when a concurrent empty probe already committed";
}

TEST(TransactionBehaviorTests, SerializableSnapshotIsolationAbortsUpdateIntoPredicate) {
    // Updating a row into a predicate range is treated like an insert for phantom SSI.
    // T1 records the predicate without reading Bob's row id (empty high-salary set).
    TransactionManager transactions;
    Table table{"Employees",
                {{"id", ColumnType::Int}, {"name", ColumnType::String}, {"salary", ColumnType::Double}}};

    const auto bob =
        table.insert({Value{static_cast<std::int64_t>(2)}, Value{"Bob"}, Value{90000.0}},
                     transactions.beginCommitted(), &transactions);

    const auto t1 = transactions.begin();
    transactions.recordPredicateRead(
        t1.id, SsiPredicate{"Employees", "salary", ComparisonOperator::Greater, Value{100000.0}});

    const auto t2 = transactions.begin();
    ASSERT_TRUE(table.update(bob, 2, Value{150000.0}, t2.id, &transactions));
    transactions.commit(t2.id);
    EXPECT_THROW(transactions.commit(t1.id), SerializationFailure);
}

TEST(TransactionBehaviorTests, SerializableSnapshotIsolationAbortsWriteSkew) {
    // Classic on-call write skew: two txns each observe "at least one on-call", then flip
    // different rows off. Row-level SSI aborts the later committer so the invariant holds.
    TransactionManager transactions;
    Table table{"Doctors", {{"id", ColumnType::Int}, {"on_call", ColumnType::Int}}};

    const auto docA =
        table.insert({Value{static_cast<std::int64_t>(1)}, Value{static_cast<std::int64_t>(1)}},
                     transactions.beginCommitted(), &transactions);
    const auto docB =
        table.insert({Value{static_cast<std::int64_t>(2)}, Value{static_cast<std::int64_t>(1)}},
                     transactions.beginCommitted(), &transactions);

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

    ASSERT_TRUE(table.update(docA, 1, Value{static_cast<std::int64_t>(0)}, t1.id, &transactions));
    ASSERT_TRUE(table.update(docB, 1, Value{static_cast<std::int64_t>(0)}, t2.id, &transactions));
    transactions.commit(t1.id);
    EXPECT_THROW(transactions.commit(t2.id), SerializationFailure);

    EXPECT_EQ(countOnCall(transactions.currentSnapshot()), 1U)
        << "SSI aborts the second committer; one doctor remains on-call";
}

TEST(TransactionBehaviorTests, OrPredicateSireadAbortsMatchingInsertOnly) {
    // Phase 2a: OR of column comparisons records each arm as a column SIREAD (not relation
    // membership). A concurrent insert matching any arm aborts; a non-matching insert does not.
    TransactionManager transactions;
    Table table{"Employees",
                {{"id", ColumnType::Int}, {"name", ColumnType::String}, {"salary", ColumnType::Double}}};

    table.insert({Value{static_cast<std::int64_t>(1)}, Value{"Alice"}, Value{120000.0}},
                 transactions.beginCommitted(), &transactions);

    // SELECT … WHERE salary > 100000 OR id = 99 — record both arms.
    const auto readerMatch = transactions.begin();
    transactions.recordPredicateRead(
        readerMatch.id,
        SsiPredicate{"Employees", "salary", ComparisonOperator::Greater, Value{100000.0},
                     std::nullopt});
    transactions.recordPredicateRead(
        readerMatch.id, SsiPredicate{"Employees", "id", ComparisonOperator::Equal,
                                     Value{static_cast<std::int64_t>(99)}, std::nullopt});

    const auto matchingWriter = transactions.begin();
    table.insert({Value{static_cast<std::int64_t>(2)}, Value{"Bob"}, Value{110000.0}},
                 matchingWriter.id, &transactions);
    transactions.commit(matchingWriter.id);
    EXPECT_THROW(transactions.commit(readerMatch.id), SerializationFailure)
        << "OR SIREAD must abort when an insert matches any arm";

    const auto readerOk = transactions.begin();
    transactions.recordPredicateRead(
        readerOk.id, SsiPredicate{"Employees", "salary", ComparisonOperator::Greater,
                                  Value{100000.0}, std::nullopt});
    transactions.recordPredicateRead(
        readerOk.id, SsiPredicate{"Employees", "id", ComparisonOperator::Equal,
                                  Value{static_cast<std::int64_t>(99)}, std::nullopt});

    const auto nonMatchingWriter = transactions.begin();
    table.insert({Value{static_cast<std::int64_t>(3)}, Value{"Zed"}, Value{50000.0}},
                 nonMatchingWriter.id, &transactions);
    transactions.commit(nonMatchingWriter.id);
    EXPECT_NO_THROW(transactions.commit(readerOk.id))
        << "OR SIREAD must not conflict with inserts outside every arm";
}

TEST(TransactionBehaviorTests, LikePredicateSireadAbortsMatchingInsertOnly) {
    // Phase 2a: column LIKE records a LIKE SIREAD. Matching inserts abort; non-matching do not.
    TransactionManager transactions;
    Table table{"Employees",
                {{"id", ColumnType::Int}, {"name", ColumnType::String}, {"salary", ColumnType::Double}}};

    table.insert({Value{static_cast<std::int64_t>(1)}, Value{"Alice"}, Value{120000.0}},
                 transactions.beginCommitted(), &transactions);

    const auto readerMatch = transactions.begin();
    transactions.recordPredicateRead(
        readerMatch.id,
        SsiPredicate{"Employees", "name", std::nullopt, std::nullopt, std::string{"Al%"}});

    const auto matchingWriter = transactions.begin();
    table.insert({Value{static_cast<std::int64_t>(2)}, Value{"Albert"}, Value{90000.0}},
                 matchingWriter.id, &transactions);
    transactions.commit(matchingWriter.id);
    EXPECT_THROW(transactions.commit(readerMatch.id), SerializationFailure)
        << "LIKE SIREAD must abort on a pattern-matching insert";

    const auto readerOk = transactions.begin();
    transactions.recordPredicateRead(
        readerOk.id,
        SsiPredicate{"Employees", "name", std::nullopt, std::nullopt, std::string{"Al%"}});

    const auto nonMatchingWriter = transactions.begin();
    table.insert({Value{static_cast<std::int64_t>(3)}, Value{"Zed"}, Value{50000.0}},
                 nonMatchingWriter.id, &transactions);
    transactions.commit(nonMatchingWriter.id);
    EXPECT_NO_THROW(transactions.commit(readerOk.id))
        << "LIKE SIREAD must not conflict with inserts outside the pattern";
}

TEST(TransactionBehaviorTests, RegexSubqueryPredicateReadsUseRelationMembershipSiread) {
    // Documented limitation: regex / subquery scans still take relation-membership SIREADs.
    // Any concurrent insert into the relation conflicts — even a row that would not match.
    TransactionManager transactions;
    Table table{"Employees",
                {{"id", ColumnType::Int}, {"name", ColumnType::String}, {"salary", ColumnType::Double}}};

    table.insert({Value{static_cast<std::int64_t>(1)}, Value{"Alice"}, Value{120000.0}},
                 transactions.beginCommitted(), &transactions);

    const auto t1 = transactions.begin();
    transactions.recordRelationRead(t1.id, "Employees");

    const auto t2 = transactions.begin();
    table.insert({Value{static_cast<std::int64_t>(2)}, Value{"Zed"}, Value{50000.0}}, t2.id,
                 &transactions);
    transactions.commit(t2.id);
    EXPECT_THROW(transactions.commit(t1.id), SerializationFailure)
        << "relation-membership SIREAD must conflict with any insert into the relation";
}

TEST(TransactionBehaviorTests, SelectEngineOrLikeScanRecordsColumnSireads) {
    // End-to-end: SelectEngine scan recording for OR / LIKE uses column SIREADs so a
    // non-matching concurrent insert does not abort the reader.
    auto database = std::make_shared<Database>("company");
    database->createTable(
        "Employees",
        {{"id", ColumnType::Int}, {"name", ColumnType::String}, {"salary", ColumnType::Double}});
    auto &table = *database->table("Employees");

    TxnSession session;
    QueryPlanner planner;
    ExecutionContext ctx{database, planner, session};
    SelectEngine selectEngine{ctx};
    SubqueryRuntime subqueryRuntime{ctx};
    ctx.select = &selectEngine;
    ctx.subquery = &subqueryRuntime;

    table.insert({Value{static_cast<std::int64_t>(1)}, Value{"Alice"}, Value{120000.0}},
                 session.transactionManager().beginCommitted(), &session.transactionManager());

    Parser parser;
    ASSERT_TRUE(session.begin().success);

    auto orSelect = std::get<Select>(
        parser.parse("SELECT id FROM Employees WHERE salary > 100000.0 OR id = 99;"));
    auto orPlan = planner.planSelect(orSelect, table);
    (void)selectEngine.collectVisibleEntries(orSelect, table, orPlan);

    auto likeSelect =
        std::get<Select>(parser.parse("SELECT id FROM Employees WHERE name LIKE \"Al%\";"));
    auto likePlan = planner.planSelect(likeSelect, table);
    (void)selectEngine.collectVisibleEntries(likeSelect, table, likePlan);

    const auto writer = session.transactionManager().begin();
    table.insert({Value{static_cast<std::int64_t>(3)}, Value{"Zed"}, Value{50000.0}}, writer.id,
                 &session.transactionManager());
    session.transactionManager().commit(writer.id);
    const auto commit = session.commit();
    EXPECT_TRUE(commit.success) << commit.message
                                << " — SelectEngine OR/LIKE SIREADs must not conflict with a "
                                   "non-matching insert";
}

TEST(TransactionBehaviorTests, SerializableSnapshotIsolationAbortsWriteWriteConflict) {
    // First-committer wins: two txns update the same row; the later commit aborts.
    TransactionManager transactions;
    Table table{"Accounts", {{"id", ColumnType::Int}, {"balance", ColumnType::Int}}};

    const auto rowId =
        table.insert({Value{static_cast<std::int64_t>(1)}, Value{static_cast<std::int64_t>(100)}},
                     transactions.beginCommitted(), &transactions);

    const auto t1 = transactions.begin();
    const auto t2 = transactions.begin();
    (void)table.rowsSnapshot(transactions.currentSnapshot(t1.id), transactions);
    (void)table.rowsSnapshot(transactions.currentSnapshot(t2.id), transactions);

    ASSERT_TRUE(table.update(rowId, 1, Value{static_cast<std::int64_t>(90)}, t1.id, &transactions));
    ASSERT_TRUE(table.update(rowId, 1, Value{static_cast<std::int64_t>(80)}, t2.id, &transactions));
    transactions.commit(t1.id);
    EXPECT_THROW(transactions.commit(t2.id), SerializationFailure);

    auto latest = table.rowsSnapshot(transactions.currentSnapshot(), transactions);
    ASSERT_EQ(latest.size(), 1U);
    EXPECT_EQ(latest.front()[1], Value{static_cast<std::int64_t>(90)});
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
