#include "VertexDB/storage/database.hpp"
#include "VertexDB/storage/row_store.hpp"

#include <gtest/gtest.h>
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
    EXPECT_EQ(table.findIndexed("id", Value{1}), std::vector<RowId>{first});

    ASSERT_TRUE(table.update(first, 0, Value{3}));
    EXPECT_TRUE(table.findIndexed("id", Value{1}).empty());
    EXPECT_EQ(table.findIndexed("id", Value{3}), std::vector<RowId>{first});

    ASSERT_TRUE(table.erase(first));
    EXPECT_TRUE(table.findIndexed("id", Value{3}).empty());
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
    Table table{"Employees", {{"id", ColumnType::Int}, {"name", ColumnType::String}}};

    const auto rowId = table.insert({Value{1}, Value{std::string{"Alice"}}});
    ASSERT_TRUE(table.update(rowId, 1, Value{std::string{"Alicia"}}));

    auto initialView = table.rowsSnapshot(TransactionId{1});
    ASSERT_EQ(initialView.size(), 1U);
    EXPECT_EQ(initialView.front()[1], Value{std::string{"Alice"}});

    auto updatedView = table.rowsSnapshot(TransactionId{2});
    ASSERT_EQ(updatedView.size(), 1U);
    EXPECT_EQ(updatedView.front()[1], Value{std::string{"Alicia"}});

    auto byId = table.rowsById(std::vector<RowId>{rowId}, TransactionId{1});
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
    EXPECT_EQ((*store.get(0))[0], Value{20});
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
