#include "VertexDB/concurrency/lock_manager.hpp"
#include "VertexDB/indexing/btree_index.hpp"
#include "VertexDB/indexing/hash_index.hpp"
#include "VertexDB/persistence/write_ahead_log.hpp"
#include "VertexDB/planner/query_planner.hpp"
#include "VertexDB/storage/buffer_pool.hpp"
#include "VertexDB/storage/table.hpp"
#include "VertexDB/transaction/mvcc_row_store.hpp"
#include "VertexDB/transaction/transaction_manager.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <future>

namespace VertexDB {

TEST(DeepFeatureTests, BTreeIndexSupportsRangeLookup) {
    BTreeIndex index;
    index.insert(Value{1}, 10);
    index.insert(Value{2}, 20);
    index.insert(Value{3}, 30);

    EXPECT_EQ(index.find(Value{2}), std::vector<RowId>{20});
    EXPECT_EQ(index.lessThan(Value{3}), (std::vector<RowId>{10, 20}));
    EXPECT_EQ(index.greaterThan(Value{1}), (std::vector<RowId>{20, 30}));
}

TEST(DeepFeatureTests, BTreeIndexMaintainsPageLayoutMetadata) {
    BTreeIndex index{2};
    index.insert(Value{1}, 10);
    index.insert(Value{2}, 20);
    index.insert(Value{3}, 30);
    index.insert(Value{4}, 40);
    index.insert(Value{5}, 50);

    EXPECT_EQ(index.leafPageCount(), 3U);
    EXPECT_EQ(index.height(), 2U);

    const auto nodes = index.nodesSnapshot();
    ASSERT_EQ(nodes.size(), 4U);
    EXPECT_TRUE(nodes[0].leaf);
    EXPECT_EQ(nodes[0].nextLeaf, nodes[1].pageId);
    EXPECT_EQ(nodes[0].rowIds.front(), std::vector<RowId>{10});
    EXPECT_FALSE(nodes.back().leaf);
    EXPECT_EQ(nodes.back().children.size(), 3U);
    EXPECT_EQ(nodes.back().keys.size(), 2U);
}

TEST(DeepFeatureTests, BTreeIndexReadsFromLeafPayloadsAfterMutation) {
    BTreeIndex index{2};
    index.insert(Value{1}, 10);
    index.insert(Value{1}, 11);
    index.insert(Value{2}, 20);
    index.insert(Value{3}, 30);

    index.remove(Value{1}, 10);

    EXPECT_EQ(index.find(Value{1}), std::vector<RowId>{11});
    EXPECT_EQ(index.lessThan(Value{3}), (std::vector<RowId>{11, 20}));
    EXPECT_EQ(index.greaterThan(Value{1}), (std::vector<RowId>{20, 30}));
}

TEST(DeepFeatureTests, BTreeIndexRemovesClearsAndHandlesMissingKeys) {
    BTreeIndex index;
    index.insert(Value{1}, 10);
    index.insert(Value{1}, 11);
    index.insert(Value{2}, 20);

    index.remove(Value{1}, 10);
    EXPECT_EQ(index.find(Value{1}), std::vector<RowId>{11});
    EXPECT_TRUE(index.find(Value{99}).empty());
    EXPECT_EQ(index.size(), 2U);

    index.remove(Value{1}, 11);
    EXPECT_TRUE(index.find(Value{1}).empty());
    index.remove(Value{42}, 1);
    index.clear();
    EXPECT_EQ(index.size(), 0U);
}

TEST(DeepFeatureTests, HashIndexRemovesClearsAndSupportsStringKeys) {
    HashIndex index;
    index.insert(Value{std::string{"alpha"}}, 1);
    index.insert(Value{std::string{"alpha"}}, 2);
    index.insert(Value{42}, 3);

    index.remove(Value{std::string{"alpha"}}, 1);
    EXPECT_EQ(index.find(Value{std::string{"alpha"}}), std::vector<RowId>{2});
    EXPECT_EQ(index.size(), 2U);

    index.remove(Value{std::string{"alpha"}}, 2);
    EXPECT_TRUE(index.find(Value{std::string{"alpha"}}).empty());
    index.remove(Value{std::string{"missing"}}, 1);
    index.clear();
    EXPECT_EQ(index.size(), 0U);
}

TEST(DeepFeatureTests, BufferPoolEvictsLeastRecentlyUsedPage) {
    BufferPool pool{2};
    pool.put(Page{1, {std::byte{1}}, false});
    pool.put(Page{2, {std::byte{2}}, false});
    ASSERT_TRUE(pool.get(1).has_value());
    pool.put(Page{3, {std::byte{3}}, false});

    EXPECT_TRUE(pool.contains(1));
    EXPECT_FALSE(pool.contains(2));
    EXPECT_TRUE(pool.contains(3));
}

TEST(DeepFeatureTests, BufferPoolRejectsInvalidCapacityAndUpdatesExistingPages) {
    EXPECT_THROW(BufferPool{0}, std::invalid_argument);

    BufferPool pool{2};
    pool.put(Page{1, {std::byte{1}}, false});
    pool.put(Page{1, {std::byte{9}}, true});

    auto page = pool.get(1);
    ASSERT_TRUE(page.has_value());
    ASSERT_EQ(page->bytes.size(), 1U);
    EXPECT_EQ(page->bytes.front(), std::byte{9});
    EXPECT_TRUE(page->dirty);
    EXPECT_EQ(pool.size(), 1U);
    EXPECT_EQ(pool.capacity(), 2U);
    EXPECT_FALSE(pool.get(99).has_value());
}

TEST(DeepFeatureTests, LockManagerAllowsSharedReadersAndBlocksWriters) {
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

TEST(DeepFeatureTests, WriteAheadLogAppendsAndReadsRecords) {
    const auto root = std::filesystem::temp_directory_path() /
                      ("VertexDB_wal_test_" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto path = root / "test.wal";

    WriteAheadLog wal{path};
    wal.reset();
    EXPECT_EQ(wal.append(WalOperation::CreateDatabase, "company"), 1U);
    EXPECT_EQ(wal.append(WalOperation::Insert, "Employees"), 2U);

    const auto records = wal.readAll();
    ASSERT_EQ(records.size(), 2U);
    EXPECT_EQ(records[0].operation, WalOperation::CreateDatabase);
    EXPECT_EQ(records[0].payload, "company");
    EXPECT_EQ(records[1].operation, WalOperation::Insert);

    std::filesystem::remove_all(root);
}

TEST(DeepFeatureTests, WriteAheadLogContinuesLsnAfterReopen) {
    const auto root = std::filesystem::temp_directory_path() /
                      ("VertexDB_wal_reopen_test_" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto path = root / "test.wal";

    {
        WriteAheadLog wal{path};
        wal.reset();
        EXPECT_EQ(wal.append(WalOperation::CreateDatabase, "company"), 1U);
    }
    {
        WriteAheadLog wal{path};
        EXPECT_EQ(wal.append(WalOperation::Insert, "Employees"), 2U);
    }

    std::filesystem::remove_all(root);
}

TEST(DeepFeatureTests, PlannerChoosesIndexAccessPaths) {
    Table table{"Employees", {{"id", ColumnType::Int}, {"salary", ColumnType::Double}}};
    table.insert({Value{1}, Value{100000.0}});
    table.insert({Value{2}, Value{120000.0}});
    table.insert({Value{3}, Value{90000.0}});
    ASSERT_TRUE(table.createIndex("idx_id", "id"));
    ASSERT_TRUE(table.createIndex("idx_salary", "salary"));

    QueryPlanner planner;
    Select equality{"Employees", std::nullopt,
                    {"*"},       Predicate{"id", ComparisonOperator::Equal, Value{1}},
                    {},          {}};
    const auto equalityPlan = planner.planSelect(equality, table);
    EXPECT_EQ(equalityPlan.accessPath, AccessPath::HashIndexLookup);
    EXPECT_EQ(equalityPlan.estimatedRows, 1U);
    EXPECT_LT(equalityPlan.estimatedCost, static_cast<double>(table.rowCount()));

    Select range{"Employees", std::nullopt,
                 {"*"},       Predicate{"salary", ComparisonOperator::Greater, Value{100000.0}},
                 {},          {}};
    const auto rangePlan = planner.planSelect(range, table);
    EXPECT_EQ(rangePlan.accessPath, AccessPath::OrderedIndexRange);
    EXPECT_GE(rangePlan.estimatedRows, 1U);
}

TEST(DeepFeatureTests, MVCCTracksTransactionVisibility) {
    TransactionManager transactions;
    const auto writer = transactions.begin();
    const auto reader = transactions.begin();

    MVCCRowStore store;
    store.write(1, {Value{10}}, writer.id);
    EXPECT_TRUE(store.read(1, reader.id).has_value());

    store.erase(1, reader.id);
    EXPECT_FALSE(store.read(1, reader.id).has_value());
    EXPECT_EQ(store.versionCount(1), 1U);
}

TEST(DeepFeatureTests, TransactionManagerTracksCommitRollbackAndInvalidTransitions) {
    TransactionManager transactions;
    const auto first = transactions.begin();
    const auto second = transactions.begin();

    transactions.commit(first.id);
    ASSERT_TRUE(transactions.find(first.id).has_value());
    EXPECT_EQ(transactions.find(first.id)->state, TransactionState::Committed);

    transactions.rollback(second.id);
    ASSERT_TRUE(transactions.find(second.id).has_value());
    EXPECT_EQ(transactions.find(second.id)->state, TransactionState::RolledBack);

    EXPECT_FALSE(transactions.find(999).has_value());
    EXPECT_THROW(transactions.commit(first.id), std::runtime_error);
    EXPECT_THROW(transactions.rollback(second.id), std::runtime_error);
    EXPECT_THROW(transactions.commit(999), std::runtime_error);
    EXPECT_THROW(transactions.rollback(999), std::runtime_error);
}

} // namespace VertexDB
