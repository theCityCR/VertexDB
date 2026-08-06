#include "VertexDB/storage/database.hpp"
#include "VertexDB/storage/row_store.hpp"
#include "VertexDB/persistence/storage_manager.hpp"

#include <gtest/gtest.h>
#include <filesystem>
#include <thread>
#include <vector>

namespace VertexDB {

TEST(StorageTests, CreatesTableAndInsertsTypedRows) {
    Database database{"company"};
    ASSERT_TRUE(
        database.createTable("Employees", {{"id", ColumnType::Int}, {"name", ColumnType::String}}));

    auto table = database.table("Employees");
    ASSERT_NE(table, nullptr);
    table->insert({Value{1}, Value{std::string{"Alice"}}});

    EXPECT_EQ(table->rowCount(), 1U);
}

TEST(StorageTests, RejectsRowsWithWrongShape) {
    Table table{"Employees", {{"id", ColumnType::Int}}};

    EXPECT_THROW(table.insert({Value{1}, Value{2}}), std::invalid_argument);
}

TEST(StorageTests, SupportsNullableColumns) {
    Table table{"Employees", {{"id", ColumnType::Int}, {"nickname", ColumnType::String, true}}};
    EXPECT_NO_THROW(table.insert({Value{1}, Value{}}));

    Table strict{"Employees", {{"id", ColumnType::Int}, {"nickname", ColumnType::String}}};
    EXPECT_THROW(strict.insert({Value{1}, Value{}}), std::invalid_argument);
}

TEST(StorageTests, MaintainsIndexesAcrossInsertUpdateAndDelete) {
    Table table{"Employees", {{"id", ColumnType::Int}, {"name", ColumnType::String}}};
    ASSERT_TRUE(table.createIndex("idx_id", "id"));

    const auto first = table.insert({Value{1}, Value{std::string{"Alice"}}});
    table.insert({Value{2}, Value{std::string{"Bob"}}});
    EXPECT_EQ(table.indexedLookup("id", Value{1}).value_or(std::vector<RowId>{}), std::vector<RowId>{first});

    ASSERT_TRUE(table.update(first, 0, Value{3}));
    EXPECT_TRUE(table.indexedLookup("id", Value{1}).value_or(std::vector<RowId>{}).empty());
    EXPECT_EQ(table.indexedLookup("id", Value{3}).value_or(std::vector<RowId>{}), std::vector<RowId>{first});

    ASSERT_TRUE(table.erase(first));
    EXPECT_TRUE(table.indexedLookup("id", Value{3}).value_or(std::vector<RowId>{}).empty());
}

TEST(StorageTests, TracksRowVersionsAcrossTableMutations) {
    Table table{"Employees", {{"id", ColumnType::Int}, {"name", ColumnType::String}}};

    const auto rowId = table.insert({Value{1}, Value{std::string{"Alice"}}});
    EXPECT_EQ(table.versionCount(rowId), 1U);

    ASSERT_TRUE(table.update(rowId, 1, Value{std::string{"Alicia"}}));
    EXPECT_EQ(table.versionCount(rowId), 2U);

    ASSERT_TRUE(table.erase(rowId));
    EXPECT_EQ(table.versionCount(rowId), 2U);
}

TEST(StorageTests, ProvidesTransactionVisibleSnapshotsThroughMvccBoundary) {
    TransactionManager transactions;
    Table table{"Employees", {{"id", ColumnType::Int}, {"name", ColumnType::String}}};

    const auto firstWriter = transactions.begin();
    const auto rowId = table.insert({Value{1}, Value{std::string{"Alice"}}}, firstWriter.id);
    transactions.commit(firstWriter.id);
    const auto afterInsert = transactions.currentSnapshot();

    const auto secondWriter = transactions.begin();
    ASSERT_TRUE(table.update(rowId, 1, Value{std::string{"Alicia"}}, secondWriter.id));
    transactions.commit(secondWriter.id);

    auto initialView = table.rowsSnapshot(afterInsert, transactions);
    ASSERT_EQ(initialView.size(), 1U);
    EXPECT_EQ(initialView.front()[1], Value{std::string{"Alice"}});

    auto updatedView = table.rowsSnapshot(transactions.currentSnapshot(), transactions);
    ASSERT_EQ(updatedView.size(), 1U);
    EXPECT_EQ(updatedView.front()[1], Value{std::string{"Alicia"}});

    auto byId = table.rowsById(std::vector<RowId>{rowId}, afterInsert, transactions);
    ASSERT_EQ(byId.size(), 1U);
    EXPECT_EQ(byId.front()[1], Value{std::string{"Alice"}});
}

TEST(StorageTests, PageRowStoreStoresRowsAcrossBufferPages) {
    PageRowStore store{2, 2};

    const auto first = store.append({Value{1}, Value{std::string{"Alice"}}});
    const auto second = store.append({Value{2}, Value{std::string{"Bob"}}});
    const auto third = store.append({Value{3}, Value{std::string{"Cara"}}});

    EXPECT_EQ(first, 0U);
    EXPECT_EQ(second, 1U);
    EXPECT_EQ(third, 2U);
    ASSERT_EQ(store.size(), 3U);
    ASSERT_NE(store.get(third), nullptr);
    EXPECT_EQ((*store.get(third))[1], Value{std::string{"Cara"}});

    ASSERT_TRUE(store.update(second, {Value{20}, Value{std::string{"Bobby"}}}));
    EXPECT_EQ((*store.get(second))[0], Value{20});
    EXPECT_EQ(store.rowsById(std::vector<RowId>{first, third}).size(), 2U);

    ASSERT_TRUE(store.erase(first));
    ASSERT_EQ(store.size(), 2U);
    ASSERT_EQ(store.capacity(), 3U);
    EXPECT_EQ(store.get(first), nullptr);
    ASSERT_NE(store.get(second), nullptr);
    EXPECT_EQ((*store.get(second))[0], Value{20});
    ASSERT_NE(store.get(third), nullptr);
    EXPECT_EQ((*store.get(third))[0], Value{3});
}

TEST(StorageTests, StableRowIdsSurviveMiddleDeleteAndReuseFreeList) {
    VectorRowStore vectorStore;
    PageRowStore pageStore{2, 4};

    for (auto *store : std::initializer_list<RowStore *>{&vectorStore, &pageStore}) {
        const auto first = store->append({Value{1}});
        const auto second = store->append({Value{2}});
        const auto third = store->append({Value{3}});

        ASSERT_TRUE(store->erase(second));
        EXPECT_EQ(store->size(), 2U);
        EXPECT_EQ(store->capacity(), 3U);
        EXPECT_EQ(store->get(second), nullptr);
        ASSERT_NE(store->get(first), nullptr);
        ASSERT_NE(store->get(third), nullptr);
        EXPECT_EQ((*store->get(first))[0], Value{1});
        EXPECT_EQ((*store->get(third))[0], Value{3});

        const auto reused = store->append({Value{4}});
        EXPECT_EQ(reused, second);
        EXPECT_EQ(store->size(), 3U);
        EXPECT_EQ(store->capacity(), 3U);
        ASSERT_NE(store->get(reused), nullptr);
        EXPECT_EQ((*store->get(reused))[0], Value{4});

        const auto entries = store->liveEntries();
        ASSERT_EQ(entries.size(), 3U);
        EXPECT_EQ(entries[0].first, first);
        EXPECT_EQ(entries[1].first, reused);
        EXPECT_EQ(entries[2].first, third);
    }
}

TEST(StorageTests, TableIndexesTrackStableRowIdsAfterMiddleDelete) {
    Table table{"Employees", {{"id", ColumnType::Int}, {"name", ColumnType::String}}};
    ASSERT_TRUE(table.createIndex("idx_id", "id"));

    const auto first = table.insert({Value{1}, Value{std::string{"Alice"}}});
    const auto second = table.insert({Value{2}, Value{std::string{"Bob"}}});
    const auto third = table.insert({Value{3}, Value{std::string{"Cara"}}});

    ASSERT_TRUE(table.erase(second));
    EXPECT_EQ(table.rowCount(), 2U);
    EXPECT_EQ(table.capacity(), 3U);
    EXPECT_TRUE(table.indexedLookup("id", Value{2}).value_or(std::vector<RowId>{}).empty());
    EXPECT_EQ(table.indexedLookup("id", Value{1}).value_or(std::vector<RowId>{}), std::vector<RowId>{first});
    EXPECT_EQ(table.indexedLookup("id", Value{3}).value_or(std::vector<RowId>{}), std::vector<RowId>{third});

    const auto reused = table.insert({Value{4}, Value{std::string{"Dana"}}});
    EXPECT_EQ(reused, second);
    EXPECT_EQ(table.indexedLookup("id", Value{4}).value_or(std::vector<RowId>{}), std::vector<RowId>{reused});
}

TEST(StorageTests, SaveLoadPreservesSparseIndexesAndFreeListReuse) {
    const auto root =
        std::filesystem::temp_directory_path() /
        ("VertexDB_sparse_index_roundtrip_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::remove_all(root);
    StorageManager storage{root};

    RowId keptId = 0;
    RowId deletedId = 0;
    {
        Database database{"company"};
        ASSERT_TRUE(
            database.createTable("Employees", {{"id", ColumnType::Int}, {"name", ColumnType::String}}));
        auto table = database.table("Employees");
        ASSERT_TRUE(table->createIndex("idx_id", "id"));
        (void)table->insert({Value{static_cast<std::int64_t>(1)}, Value{std::string{"Alice"}}});
        (void)table->insert({Value{static_cast<std::int64_t>(2)}, Value{std::string{"Bob"}}});
        (void)table->insert({Value{static_cast<std::int64_t>(3)}, Value{std::string{"Cara"}}});
        const auto beforeDelete = table->liveEntries();
        ASSERT_EQ(beforeDelete.size(), 3U);
        deletedId = beforeDelete[1].first;
        keptId = beforeDelete[2].first;
        ASSERT_TRUE(table->erase(deletedId));
        storage.saveDatabase(database);
    }

    auto loaded = storage.loadDatabase("company");
    auto table = loaded->table("Employees");
    ASSERT_NE(table, nullptr);
    ASSERT_TRUE(table->hasIndex("id"));
    EXPECT_EQ(table->indexedLookup("id", Value{static_cast<std::int64_t>(3)})
                  .value_or(std::vector<RowId>{}),
              std::vector<RowId>{keptId});
    const auto reused =
        table->insert({Value{static_cast<std::int64_t>(4)}, Value{std::string{"Dana"}}});
    EXPECT_EQ(reused, deletedId);
    EXPECT_EQ(table->indexedLookup("id", Value{static_cast<std::int64_t>(4)})
                  .value_or(std::vector<RowId>{}),
              std::vector<RowId>{deletedId});

    std::filesystem::remove_all(root);
}

TEST(StorageTests, ReplaceSparseRestoresCapacityFreeListAndLiveIds) {
    for (auto makeStore : {makeVectorRowStore, makePageRowStore}) {
        auto store = makeStore();
        const auto first = store->append({Value{1}});
        const auto second = store->append({Value{2}});
        const auto third = store->append({Value{3}});
        ASSERT_TRUE(store->erase(second));

        auto restored = makeStore();
        restored->replaceSparse(store->capacity(), store->freeList(), store->liveEntries());

        EXPECT_EQ(restored->capacity(), 3U);
        EXPECT_EQ(restored->size(), 2U);
        EXPECT_EQ(restored->freeList(), std::vector<RowId>{second});
        EXPECT_EQ(restored->get(second), nullptr);
        ASSERT_NE(restored->get(first), nullptr);
        ASSERT_NE(restored->get(third), nullptr);
        EXPECT_EQ((*restored->get(first))[0], Value{1});
        EXPECT_EQ((*restored->get(third))[0], Value{3});

        const auto reused = restored->append({Value{4}});
        EXPECT_EQ(reused, second);
    }
}

TEST(StorageTests, ReviveRestoresExactRowIdFromFreeList) {
    for (auto makeStore : {makeVectorRowStore, makePageRowStore}) {
        auto store = makeStore();
        const auto first = store->append({Value{1}});
        const auto second = store->append({Value{2}});
        ASSERT_TRUE(store->erase(first));
        EXPECT_EQ(store->get(first), nullptr);

        ASSERT_TRUE(store->revive(first, {Value{11}}));
        ASSERT_NE(store->get(first), nullptr);
        EXPECT_EQ((*store->get(first))[0], Value{11});
        EXPECT_EQ(store->size(), 2U);
        EXPECT_TRUE(store->freeList().empty());
        EXPECT_EQ(store->get(second)->front(), Value{2});
        EXPECT_FALSE(store->revive(second, {Value{99}}));
    }
}

TEST(StorageTests, SupportsConcurrentInserts) {
    Table table{"Events", {{"id", ColumnType::Int}}};
    constexpr int threadCount = 4;
    constexpr int insertsPerThread = 250;

    std::vector<std::thread> threads;
    for (int thread = 0; thread < threadCount; ++thread) {
        threads.emplace_back([thread, &table] {
            for (int i = 0; i < insertsPerThread; ++i) {
                table.insert({Value{thread * insertsPerThread + i}});
            }
        });
    }

    for (auto &thread : threads) {
        thread.join();
    }
    EXPECT_EQ(table.rowCount(), static_cast<std::size_t>(threadCount * insertsPerThread));
}

} // namespace VertexDB
