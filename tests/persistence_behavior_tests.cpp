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
        std::filesystem::temp_directory_path() / ("vertexdb-persistence-" + std::string(suffix));
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

TEST(PersistenceBehaviorTests, PageRowStoreMirrorsSerializedPagesIntoBufferPool) {
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

TEST(PersistenceBehaviorTests, PageRowStoreReadsLiveRowsFromPagePayloadBytes) {
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

TEST(PersistenceBehaviorTests, SaveLoadPersistsPageDirectoryPayloadBytes) {
    // Desired: snapshots write PageRowStore directory bytes verbatim (format v3+), not sparse rows.
    const auto root =
        std::filesystem::temp_directory_path() /
        ("vertexdb-desired-page-payload-save-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::remove_all(root);
    StorageManager storage{root};

    PageStoreSnapshot before;
    RowId deletedId = 0;
    {
        Database database{"company"};
        ASSERT_TRUE(database.createTable(
            "Employees", {{"id", ColumnType::Int}, {"name", ColumnType::String}}));
        auto table = database.table("Employees");
        ASSERT_TRUE(table->createIndex("idx_id", "id"));
        (void)table->insert({Value{static_cast<std::int64_t>(1)}, Value{std::string{"Alice"}}});
        (void)table->insert({Value{static_cast<std::int64_t>(2)}, Value{std::string{"Bob"}}});
        (void)table->insert({Value{static_cast<std::int64_t>(3)}, Value{std::string{"Cara"}}});
        const auto entries = table->liveEntries();
        ASSERT_EQ(entries.size(), 3U);
        deletedId = entries[1].first;
        ASSERT_TRUE(table->erase(deletedId));
        before = table->exportPageStore();
        ASSERT_FALSE(before.pages.empty());
        storage.saveDatabase(database);
    }

    auto loaded = storage.loadDatabase("company");
    auto table = loaded->table("Employees");
    ASSERT_NE(table, nullptr);
    const auto after = table->exportPageStore();
    EXPECT_EQ(after.rowsPerPage, before.rowsPerPage);
    EXPECT_EQ(after.capacity, before.capacity);
    EXPECT_EQ(after.freeList, before.freeList);
    ASSERT_EQ(after.pages.size(), before.pages.size());
    for (std::size_t index = 0; index < before.pages.size(); ++index) {
        EXPECT_EQ(after.pages[index].first, before.pages[index].first);
        EXPECT_EQ(after.pages[index].second, before.pages[index].second);
    }

    EXPECT_EQ(table->indexedLookup("id", Value{static_cast<std::int64_t>(3)})
                  .value_or(std::vector<RowId>{}),
              std::vector<RowId>{2});
    const auto reused =
        table->insert({Value{static_cast<std::int64_t>(4)}, Value{std::string{"Dana"}}});
    EXPECT_EQ(reused, deletedId);

    std::filesystem::remove_all(root);
}

TEST(PersistenceBehaviorTests, SaveLoadPersistsBTreeAndHashIndexPagesWithoutRebuild) {
    // Desired (v4): index pages round-trip; B-tree nodesSnapshot and hash lookups match pre-save.
    const auto root =
        std::filesystem::temp_directory_path() /
        ("vertexdb-desired-index-pages-v4-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::remove_all(root);
    StorageManager storage{root};

    std::vector<BTreeNode> beforeNodes;
    {
        Database database{"company"};
        ASSERT_TRUE(database.createTable(
            "Employees", {{"id", ColumnType::Int}, {"name", ColumnType::String}}));
        auto table = database.table("Employees");
        ASSERT_TRUE(table->createIndex("idx_id", "id"));
        for (int i = 1; i <= 20; ++i) {
            (void)table->insert(
                {Value{static_cast<std::int64_t>(i)}, Value{"E" + std::to_string(i)}});
        }
        ASSERT_TRUE(table->erase(5));
        ASSERT_TRUE(table->erase(12));
        beforeNodes = table->orderedIndexNodesSnapshot("idx_id").value_or(std::vector<BTreeNode>{});
        ASSERT_FALSE(beforeNodes.empty());
        storage.saveDatabase(database);
    }

    auto loaded = storage.loadDatabase("company");
    auto table = loaded->table("Employees");
    ASSERT_NE(table, nullptr);
    const auto afterNodes =
        table->orderedIndexNodesSnapshot("idx_id").value_or(std::vector<BTreeNode>{});
    ASSERT_EQ(afterNodes.size(), beforeNodes.size());
    for (std::size_t i = 0; i < beforeNodes.size(); ++i) {
        EXPECT_EQ(afterNodes[i].pageId, beforeNodes[i].pageId);
        EXPECT_EQ(afterNodes[i].leaf, beforeNodes[i].leaf);
        EXPECT_EQ(afterNodes[i].keys, beforeNodes[i].keys);
        EXPECT_EQ(afterNodes[i].rowIds, beforeNodes[i].rowIds);
        EXPECT_EQ(afterNodes[i].children, beforeNodes[i].children);
        EXPECT_EQ(afterNodes[i].nextLeaf, beforeNodes[i].nextLeaf);
    }
    EXPECT_EQ(table->indexedLookup("id", Value{static_cast<std::int64_t>(3)})
                  .value_or(std::vector<RowId>{}),
              std::vector<RowId>{2});
    EXPECT_TRUE(table->indexedLookup("id", Value{static_cast<std::int64_t>(6)})
                    .value_or(std::vector<RowId>{})
                    .empty());
    EXPECT_EQ(table->orderedLookup("id", ComparisonOperator::Less, Value{static_cast<std::int64_t>(4)})
                  .value_or(std::vector<RowId>{})
                  .size(),
              3U);

    std::filesystem::remove_all(root);
}

TEST(PersistenceBehaviorTests, IndexPageRoundTripAfterDeletesAndSplits) {
    const auto root =
        std::filesystem::temp_directory_path() /
        ("vertexdb-desired-index-split-roundtrip-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::remove_all(root);
    StorageManager storage{root};

    std::vector<BTreeNode> beforeNodes;
    {
        // Force splits with a tiny fanout via direct BTreeIndex, then through Table save/load with
        // default fanout after enough keys — assert nodesSnapshot equality on the table index.
        Database database{"company"};
        ASSERT_TRUE(database.createTable("Keys", {{"id", ColumnType::Int}}));
        auto table = database.table("Keys");
        ASSERT_TRUE(table->createIndex("idx_id", "id"));
        for (int i = 1; i <= 200; ++i) {
            (void)table->insert({Value{static_cast<std::int64_t>(i)}});
        }
        for (int i = 1; i <= 200; i += 3) {
            ASSERT_TRUE(table->erase(static_cast<RowId>(i - 1)));
        }
        beforeNodes = table->orderedIndexNodesSnapshot("idx_id").value_or(std::vector<BTreeNode>{});
        ASSERT_GT(beforeNodes.size(), 1U);
        storage.saveDatabase(database);
    }

    auto loaded = storage.loadDatabase("company");
    auto table = loaded->table("Keys");
    ASSERT_NE(table, nullptr);
    const auto afterNodes =
        table->orderedIndexNodesSnapshot("idx_id").value_or(std::vector<BTreeNode>{});
    ASSERT_EQ(afterNodes.size(), beforeNodes.size());
    for (std::size_t i = 0; i < beforeNodes.size(); ++i) {
        EXPECT_EQ(afterNodes[i].pageId, beforeNodes[i].pageId);
        EXPECT_EQ(afterNodes[i].keys, beforeNodes[i].keys);
        EXPECT_EQ(afterNodes[i].rowIds, beforeNodes[i].rowIds);
        EXPECT_EQ(afterNodes[i].children, beforeNodes[i].children);
        EXPECT_EQ(afterNodes[i].nextLeaf, beforeNodes[i].nextLeaf);
    }
    EXPECT_FALSE(table->indexedLookup("id", Value{static_cast<std::int64_t>(2)})
                     .value_or(std::vector<RowId>{})
                     .empty());

    std::filesystem::remove_all(root);
}

TEST(PersistenceBehaviorTests, PageRowStoreReplaceFromPagesRestoresExactDirectoryBytes) {
    PageRowStore store{2, 8};
    (void)store.append({Value{1}});
    (void)store.append({Value{2}});
    const auto third = store.append({Value{3}});
    ASSERT_TRUE(store.erase(1));

    const auto exported = store.exportPages();
    ASSERT_EQ(exported.pages.size(), 2U);

    PageRowStore restored{4, 8}; // different rowsPerPage; replaceFromPages must adopt snapshot's
    restored.replaceFromPages(exported);
    EXPECT_EQ(restored.rowsPerPage(), 2U);
    EXPECT_EQ(restored.capacity(), 3U);
    EXPECT_EQ(restored.freeList(), std::vector<RowId>{1});
    EXPECT_EQ(restored.size(), 2U);
    ASSERT_EQ(restored.directoryBytes(1), exported.pages[0].second);
    ASSERT_EQ(restored.directoryBytes(2), exported.pages[1].second);
    ASSERT_NE(restored.get(0), nullptr);
    EXPECT_EQ((*restored.get(0))[0], Value{1});
    EXPECT_EQ(restored.get(1), nullptr);
    ASSERT_NE(restored.get(third), nullptr);
    EXPECT_EQ((*restored.get(third))[0], Value{3});
    EXPECT_EQ(restored.append({Value{4}}), 1U);
}

TEST(PersistenceBehaviorTests, PageImageRedoRecoversInsertUpdateDeleteWithoutSqlPayload) {
    const auto root =
        std::filesystem::temp_directory_path() / "vertexdb-desired-page-image-redo-recover";
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
    std::size_t pageImageCount = 0;
    for (const auto &record : records) {
        if (record.operation == WalOperation::PageImageRedo) {
            ++pageImageCount;
            EXPECT_EQ(record.payload.find("INSERT"), std::string::npos);
            EXPECT_EQ(record.payload.find("UPDATE"), std::string::npos);
            EXPECT_EQ(record.payload.find("DELETE"), std::string::npos);
        } else if (record.operation == WalOperation::Insert ||
                   record.operation == WalOperation::Update ||
                   record.operation == WalOperation::Delete) {
            ADD_FAILURE() << "DML should use PageImageRedo, not legacy logical SQL ops";
        }
    }
    EXPECT_EQ(pageImageCount, 4U);

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

// Keep the older PhysicalRedo recovery path for legacy WAL files.

TEST(PersistenceBehaviorTests, PhysicalRedoRecoversInsertUpdateDeleteWithoutSqlPayload) {
    const auto root =
        std::filesystem::temp_directory_path() / "vertexdb-desired-physical-redo-recover";
    std::filesystem::remove_all(root);
    Parser parser;

    WriteAheadLog wal{root / "VertexDB.wal"};
    wal.reset();
    (void)wal.append(WalOperation::CreateDatabase, "company");
    (void)wal.append(WalOperation::CreateTable,
                     "CREATE TABLE Employees (id INT, name STRING, salary DOUBLE);");
    const auto upsert1 = encodePhysicalRedo(PhysicalRedoRecord{
        PhysicalRedoKind::Upsert, "Employees", 0,
        Row{Value{static_cast<std::int64_t>(1)}, Value{"Alice"}, Value{120000.0}}});
    const auto upsert2 = encodePhysicalRedo(PhysicalRedoRecord{
        PhysicalRedoKind::Upsert, "Employees", 1,
        Row{Value{static_cast<std::int64_t>(2)}, Value{"Bob"}, Value{90000.0}}});
    const auto update1 = encodePhysicalRedo(PhysicalRedoRecord{
        PhysicalRedoKind::Upsert, "Employees", 0,
        Row{Value{static_cast<std::int64_t>(1)}, Value{"Alice"}, Value{150000.0}}});
    const auto erase2 =
        encodePhysicalRedo(PhysicalRedoRecord{PhysicalRedoKind::Erase, "Employees", 1, {}});
    (void)wal.append(WalOperation::PhysicalRedo, upsert1);
    (void)wal.append(WalOperation::PhysicalRedo, upsert2);
    (void)wal.append(WalOperation::PhysicalRedo, update1);
    (void)wal.append(WalOperation::PhysicalRedo, erase2);

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

TEST(PersistenceBehaviorTests, TruncatedTrailingWalRecordIsIgnoredDuringRecovery) {
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

TEST(PersistenceBehaviorTests, TruncatedWalPayloadKeepsPriorPageImageRedo) {
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
        const std::uint8_t op = static_cast<std::uint8_t>(WalOperation::PageImageRedo);
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

TEST(PersistenceBehaviorTests, TornTransactionBatchDoesNotPartiallyApply) {
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
    EXPECT_EQ(records[3].operation, WalOperation::PageImageRedo);
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

TEST(PersistenceBehaviorTests, LegacyLogicalInsertWalStillReplays) {
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

TEST(PersistenceBehaviorTests, ExpressionIndexSurvivesSaveLoad) {
    Parser parser;
    auto executor = makeExecutor("expr-index-save");
    seedEmployees(executor, parser, false, false);
    ASSERT_TRUE(
        executor.execute(parser.parse("CREATE INDEX idx_neg ON Employees((-salary));")).success);

    auto before = executor.execute(
        parser.parse("SELECT name FROM Employees WHERE (-salary) = -120000.0;"));
    ASSERT_TRUE(before.success);
    ASSERT_EQ(before.rows.size(), 1U);

    ASSERT_TRUE(executor.execute(parser.parse("SAVE DATABASE;")).success);
    const auto root =
        std::filesystem::temp_directory_path() / "vertexdb-desired-expr-index-save";
    QueryExecutor reloaded{root};
    ASSERT_TRUE(reloaded.execute(parser.parse("LOAD DATABASE company;")).success);

    auto explain = reloaded.execute(
        parser.parse("EXPLAIN SELECT name FROM Employees WHERE (-salary) = -120000.0;"));
    ASSERT_TRUE(explain.success);
    EXPECT_NE(explain.rows.front().front().toString().find("expression hash index"),
              std::string::npos);

    auto after = reloaded.execute(
        parser.parse("SELECT name FROM Employees WHERE (-salary) = -120000.0;"));
    ASSERT_TRUE(after.success);
    ASSERT_EQ(after.rows.size(), 1U);
    EXPECT_EQ(after.rows[0][0], Value{"Alice"});
}

TEST(PersistenceBehaviorTests, HistogramStatsPersistInSnapshotV4) {
    Parser parser;
    auto executor = makeExecutor("hist-persist-v4");
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(
        executor.execute(parser.parse("CREATE TABLE Employees (id INT, score INT);")).success);
    ASSERT_TRUE(executor.execute(parser.parse("CREATE INDEX idx_score ON Employees(score);")).success);
    for (int i = 1; i <= 60; ++i) {
        ASSERT_TRUE(executor
                        .execute(parser.parse("INSERT INTO Employees VALUES (" +
                                              std::to_string(i) + ", " + std::to_string(i) + ");"))
                        .success);
    }
    ASSERT_TRUE(executor.execute(parser.parse("ANALYZE TABLE Employees;")).success);
    ASSERT_TRUE(executor.execute(parser.parse("SAVE DATABASE;")).success);

    const auto root =
        std::filesystem::temp_directory_path() / "vertexdb-desired-hist-persist-v4";
    QueryExecutor loaded{root};
    ASSERT_TRUE(loaded.execute(parser.parse("LOAD DATABASE company;")).success);
    auto table = loaded.currentDatabase()->table("Employees");
    ASSERT_TRUE(table != nullptr);
    const auto hist = table->columnHistogram("score");
    ASSERT_TRUE(hist.has_value());
    EXPECT_EQ(hist->rowCount, 60U);
    EXPECT_EQ(hist->distinctCount, 60U);
    EXPECT_FALSE(hist->buckets.empty());

    auto explain =
        loaded.execute(parser.parse("EXPLAIN SELECT id FROM Employees WHERE score > 50;"));
    ASSERT_TRUE(explain.success);
    EXPECT_NE(explain.rows.front().front().toString().find("ordered index range"),
              std::string::npos);
}

TEST(PersistenceBehaviorTests, LoadDatabaseWithoutNameReloadsActiveDatabase) {
    auto executor = makeExecutor("load-active");
    Parser parser;
    ASSERT_TRUE(executor.execute(parser.parse("CREATE DATABASE company;")).success);
    ASSERT_TRUE(
        executor.execute(parser.parse("CREATE TABLE Employees (id INT, name STRING);")).success);
    ASSERT_TRUE(
        executor.execute(parser.parse("INSERT INTO Employees VALUES (1, \"Alice\");")).success);
    ASSERT_TRUE(executor.execute(parser.parse("SAVE DATABASE;")).success);
    ASSERT_TRUE(
        executor.execute(parser.parse("INSERT INTO Employees VALUES (2, \"Bob\");")).success);
    ASSERT_TRUE(executor.execute(parser.parse("LOAD DATABASE;")).success);
    auto result = executor.execute(parser.parse("SELECT id FROM Employees ORDER BY id;"));
    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.rows.size(), 1U);
    EXPECT_EQ(result.rows[0][0], Value{1});
}

} // namespace VertexDB
