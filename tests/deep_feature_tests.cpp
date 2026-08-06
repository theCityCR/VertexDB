#include "VertexDB/concurrency/lock_manager.hpp"
#include "VertexDB/indexing/btree_index.hpp"
#include "VertexDB/indexing/hash_index.hpp"
#include "VertexDB/persistence/storage_manager.hpp"
#include "VertexDB/persistence/write_ahead_log.hpp"
#include "VertexDB/planner/query_planner.hpp"
#include "VertexDB/storage/buffer_pool.hpp"
#include "VertexDB/storage/table.hpp"
#include "VertexDB/transaction/mvcc_row_store.hpp"
#include "VertexDB/transaction/transaction_manager.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <vector>

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

TEST(DeepFeatureTests, BTreeIndexIncrementalLayoutKeepsValidLinkedTreeInvariants) {
    // Fanout 2 forces leaf/internal splits; assert durable B+ invariants, not exact height.
    BTreeIndex index{2};
    index.insert(Value{1}, 10);
    index.insert(Value{2}, 20);
    index.insert(Value{3}, 30);
    index.insert(Value{4}, 40);
    index.insert(Value{5}, 50);

    EXPECT_GE(index.height(), 2U);
    EXPECT_GE(index.leafPageCount(), 2U);
    EXPECT_EQ(index.find(Value{3}), std::vector<RowId>{30});
    EXPECT_EQ(index.lessThan(Value{3}), (std::vector<RowId>{10, 20}));
    EXPECT_EQ(index.greaterThan(Value{3}), (std::vector<RowId>{40, 50}));

    const auto nodes = index.nodesSnapshot();
    ASSERT_FALSE(nodes.empty());
    std::size_t leafLinks = 0;
    for (const auto &node : nodes) {
        if (node.leaf && node.nextLeaf.has_value()) {
            ++leafLinks;
        }
        if (node.leaf) {
            EXPECT_LE(node.keys.size(), 2U);
            EXPECT_EQ(node.keys.size(), node.rowIds.size());
        } else {
            EXPECT_EQ(node.children.size(), node.keys.size() + 1);
            EXPECT_LE(node.keys.size(), 2U);
        }
    }
    EXPECT_GE(leafLinks, 1U);
    EXPECT_FALSE(nodes.back().leaf);
    EXPECT_FALSE(nodes.back().children.empty());
}

TEST(DeepFeatureTests, BTreeIndexMergeAfterDeletesCollapsesHeight) {
    BTreeIndex index{2};
    for (int key = 1; key <= 16; ++key) {
        index.insert(Value{key}, static_cast<RowId>(key));
    }
    EXPECT_GE(index.height(), 3U);
    EXPECT_EQ(index.size(), 16U);

    for (int key = 1; key <= 15; ++key) {
        index.remove(Value{key}, static_cast<RowId>(key));
    }
    EXPECT_EQ(index.size(), 1U);
    EXPECT_EQ(index.find(Value{16}), std::vector<RowId>{16});
    EXPECT_TRUE(index.lessThan(Value{16}).empty());
    EXPECT_TRUE(index.greaterThan(Value{16}).empty());

    index.remove(Value{16}, 16);
    EXPECT_EQ(index.size(), 0U);
    EXPECT_EQ(index.height(), 1U);
    EXPECT_EQ(index.leafPageCount(), 1U);
}

TEST(DeepFeatureTests, BTreeIndexRejectsCapacityBelowTwo) {
    EXPECT_THROW((void)BTreeIndex{0}, std::invalid_argument);
    EXPECT_THROW((void)BTreeIndex{1}, std::invalid_argument);
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

TEST(DeepFeatureTests, WriteAheadLogIgnoresTruncatedTrailingRecord) {
    const auto root = std::filesystem::temp_directory_path() /
                      ("VertexDB_wal_trunc_test_" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto path = root / "test.wal";

    WriteAheadLog wal{path};
    wal.reset();
    EXPECT_EQ(wal.append(WalOperation::CreateDatabase, "company"), 1U);
    EXPECT_EQ(wal.append(WalOperation::PhysicalRedo, "complete"), 2U);

    {
        std::ofstream out{path, std::ios::binary | std::ios::app};
        ASSERT_TRUE(out);
        const char torn[] = {'T', 'C', 'W'};
        out.write(torn, sizeof(torn));
    }

    const auto records = WriteAheadLog{path}.readAll();
    ASSERT_EQ(records.size(), 2U);
    EXPECT_EQ(records[1].payload, "complete");

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
    EXPECT_FALSE(equalityPlan.residual.has_value());

    Select range{"Employees", std::nullopt,
                 {"*"},       Predicate{"salary", ComparisonOperator::Greater, Value{100000.0}},
                 {},          {}};
    const auto rangePlan = planner.planSelect(range, table);
    EXPECT_EQ(rangePlan.accessPath, AccessPath::OrderedIndexRange);
    EXPECT_GE(rangePlan.estimatedRows, 1U);

    Predicate andPredicate{
        Predicate::Kind::And,
        std::make_shared<Predicate>(Predicate{"id", ComparisonOperator::Equal, Value{2}}),
        std::make_shared<Predicate>(
            Predicate{"salary", ComparisonOperator::Greater, Value{100000.0}})};
    Select compound{"Employees", std::nullopt, {"*"}, andPredicate, {}, {}};
    const auto compoundPlan = planner.planSelect(compound, table);
    EXPECT_EQ(compoundPlan.accessPath, AccessPath::HashIndexLookup);
    ASSERT_TRUE(compoundPlan.residual.has_value());
    EXPECT_EQ(compoundPlan.residual->kind, Predicate::Kind::Comparison);
    EXPECT_EQ(compoundPlan.residual->column, "salary");
}

TEST(DeepFeatureTests, MVCCTracksTransactionVisibility) {
    TransactionManager transactions;
    const auto writer = transactions.begin();
    MVCCRowStore store;
    store.write(1, {Value{10}}, writer.id);

    const auto dirtyReader = transactions.currentSnapshot();
    EXPECT_FALSE(store.read(1, dirtyReader, transactions).has_value());

    transactions.commit(writer.id);
    const auto afterCommit = transactions.currentSnapshot();
    ASSERT_TRUE(store.read(1, afterCommit, transactions).has_value());
    EXPECT_EQ(*store.read(1, afterCommit, transactions), (Row{Value{10}}));

    const auto deleter = transactions.begin();
    store.erase(1, deleter.id);
    EXPECT_TRUE(store.read(1, afterCommit, transactions).has_value());
    EXPECT_FALSE(store.read(1, transactions.currentSnapshot(deleter.id), transactions).has_value());

    transactions.commit(deleter.id);
    EXPECT_FALSE(store.read(1, transactions.currentSnapshot(), transactions).has_value());
    EXPECT_EQ(store.versionCount(1), 1U);
}

TEST(DeepFeatureTests, TransactionManagerTracksCommitRollbackAndInvalidTransitions) {
    TransactionManager transactions;
    const auto first = transactions.begin();
    const auto second = transactions.begin();

    EXPECT_EQ(transactions.currentCommitSeq(), 0U);
    transactions.commit(first.id);
    ASSERT_TRUE(transactions.find(first.id).has_value());
    EXPECT_EQ(transactions.find(first.id)->state, TransactionState::Committed);
    ASSERT_TRUE(transactions.find(first.id)->commitSeq.has_value());
    EXPECT_EQ(*transactions.find(first.id)->commitSeq, 1U);
    EXPECT_EQ(transactions.currentCommitSeq(), 1U);

    const auto autocommit = transactions.beginCommitted();
    ASSERT_TRUE(transactions.find(autocommit).has_value());
    EXPECT_EQ(transactions.find(autocommit)->state, TransactionState::Committed);
    EXPECT_EQ(transactions.currentCommitSeq(), 2U);

    transactions.rollback(second.id);
    ASSERT_TRUE(transactions.find(second.id).has_value());
    EXPECT_EQ(transactions.find(second.id)->state, TransactionState::RolledBack);
    EXPECT_FALSE(transactions.find(second.id)->commitSeq.has_value());

    EXPECT_FALSE(transactions.find(999).has_value());
    EXPECT_THROW(transactions.commit(first.id), std::runtime_error);
    EXPECT_THROW(transactions.rollback(second.id), std::runtime_error);
    EXPECT_THROW(transactions.commit(999), std::runtime_error);
    EXPECT_THROW(transactions.rollback(999), std::runtime_error);
}

TEST(DeepFeatureTests, StorageManagerLoadsLegacyDenseSnapshots) {
    const auto root =
        std::filesystem::temp_directory_path() /
        ("VertexDB_v1_snapshot_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);

    {
        std::ofstream out{root / "legacy.tcrdb", std::ios::binary};
        ASSERT_TRUE(out);

        const auto writePod = [&](const auto &value) {
            out.write(reinterpret_cast<const char *>(&value), sizeof(value));
        };
        const auto writeString = [&](std::string_view value) {
            writePod(static_cast<std::uint64_t>(value.size()));
            out.write(value.data(), static_cast<std::streamsize>(value.size()));
        };

        out.write("TCRDB001", 8);
        writePod(std::uint32_t{1});
        writeString("legacy");
        writePod(std::uint64_t{1}); // table count
        writeString("Employees");
        writePod(std::uint64_t{1}); // column count
        writeString("id");
        writePod(std::uint8_t{0}); // INT
        writePod(std::uint8_t{0}); // not nullable
        writePod(std::uint64_t{0}); // indexes
        writePod(std::uint64_t{2}); // dense rows
        writePod(std::uint8_t{0});
        writePod(std::int64_t{10});
        writePod(std::uint8_t{0});
        writePod(std::int64_t{20});
    }

    StorageManager storage{root};
    auto database = storage.loadDatabase("legacy");
    auto table = database->table("Employees");
    ASSERT_NE(table, nullptr);
    EXPECT_EQ(table->capacity(), 2U);
    EXPECT_EQ(table->rowCount(), 2U);
    EXPECT_TRUE(table->freeList().empty());
    ASSERT_NE(table->liveEntries().size(), 0U);
    EXPECT_EQ(table->liveEntries()[0].first, 0U);
    EXPECT_EQ(table->liveEntries()[1].first, 1U);

    std::filesystem::remove_all(root);
}

} // namespace VertexDB
