#include "VertexDB/concurrency/lock_manager.hpp"
#include "VertexDB/indexing/btree_index.hpp"
#include "VertexDB/indexing/hash_index.hpp"
#include "VertexDB/persistence/storage_manager.hpp"
#include "VertexDB/persistence/write_ahead_log.hpp"
#include "VertexDB/planner/query_planner.hpp"
#include "VertexDB/storage/buffer_pool.hpp"
#include "VertexDB/storage/foreign_key.hpp"
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
#include <string>
#include <string_view>
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
    Select equality{"Employees", {},
                    {SelectExpr::makeStar()},
                    makeComparison("id", ComparisonOperator::Equal, Value{1}),
                    {},          {}};
    const auto equalityPlan = planner.planSelect(equality, table);
    EXPECT_EQ(equalityPlan.accessPath(), AccessPath::HashEq);
    EXPECT_EQ(equalityPlan.estimates.estimatedRows, 1U);
    EXPECT_LT(equalityPlan.estimates.estimatedCost, static_cast<double>(table.rowCount()));
    EXPECT_FALSE(equalityPlan.residual().has_value());

    Select range{"Employees", {},
                 {SelectExpr::makeStar()},
                 makeComparison("salary", ComparisonOperator::Greater, Value{100000.0}),
                 {},          {}};
    const auto rangePlan = planner.planSelect(range, table);
    EXPECT_EQ(rangePlan.accessPath(), AccessPath::OrderedRange);
    EXPECT_GE(rangePlan.estimates.estimatedRows, 1U);

    Predicate andPredicate =
        makeAnd(makeComparison("id", ComparisonOperator::Equal, Value{2}),
                makeComparison("salary", ComparisonOperator::Greater, Value{100000.0}));
    Select compound{"Employees", {}, {SelectExpr::makeStar()}, andPredicate, {}, {}};
    const auto compoundPlan = planner.planSelect(compound, table);
    EXPECT_EQ(compoundPlan.accessPath(), AccessPath::HashEq);
    ASSERT_TRUE(compoundPlan.residual().has_value());
    EXPECT_EQ(predicateKind(*compoundPlan.residual()), PredicateKind::Comparison);
    EXPECT_EQ(std::get<ComparisonPred>(*compoundPlan.residual()).column, "salary");
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

TEST(DeepFeatureTests, StorageManagerLoadsLegacySparseSnapshots) {
    const auto root =
        std::filesystem::temp_directory_path() /
        ("VertexDB_v2_snapshot_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);

    {
        std::ofstream out{root / "legacy_sparse.tcrdb", std::ios::binary};
        ASSERT_TRUE(out);

        const auto writePod = [&](const auto &value) {
            out.write(reinterpret_cast<const char *>(&value), sizeof(value));
        };
        const auto writeString = [&](std::string_view value) {
            writePod(static_cast<std::uint64_t>(value.size()));
            out.write(value.data(), static_cast<std::streamsize>(value.size()));
        };

        out.write("TCRDB001", 8);
        writePod(std::uint32_t{2}); // legacy sparse format
        writeString("legacy_sparse");
        writePod(std::uint64_t{1}); // table count
        writeString("Employees");
        writePod(std::uint64_t{1}); // column count
        writeString("id");
        writePod(std::uint8_t{0}); // INT
        writePod(std::uint8_t{0}); // not nullable
        writePod(std::uint64_t{0}); // indexes
        writePod(std::uint64_t{3}); // capacity
        writePod(std::uint64_t{1}); // free count
        writePod(std::uint64_t{1}); // free row id 1
        writePod(std::uint64_t{2}); // live count
        writePod(std::uint64_t{0}); // live row id 0
        writePod(std::uint8_t{0});
        writePod(std::int64_t{10});
        writePod(std::uint64_t{2}); // live row id 2
        writePod(std::uint8_t{0});
        writePod(std::int64_t{30});
    }

    StorageManager storage{root};
    auto database = storage.loadDatabase("legacy_sparse");
    auto table = database->table("Employees");
    ASSERT_NE(table, nullptr);
    EXPECT_EQ(table->capacity(), 3U);
    EXPECT_EQ(table->rowCount(), 2U);
    EXPECT_EQ(table->freeList(), std::vector<RowId>{1});
    ASSERT_NE(table->getRow(0), std::nullopt);
    EXPECT_EQ((*table->getRow(0))[0], Value{static_cast<std::int64_t>(10)});
    EXPECT_EQ(table->getRow(1), std::nullopt);
    ASSERT_NE(table->getRow(2), std::nullopt);
    EXPECT_EQ((*table->getRow(2))[0], Value{static_cast<std::int64_t>(30)});
    EXPECT_EQ(table->insert({Value{static_cast<std::int64_t>(40)}}), 1U);

    std::filesystem::remove_all(root);
}

TEST(DeepFeatureTests, StorageManagerLoadsLegacyPagePayloadV3Snapshots) {
    // v3: page-directory payloads without durable index pages; indexes rebuild from rows on load.
    const auto root =
        std::filesystem::temp_directory_path() /
        ("VertexDB_v3_snapshot_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);

    PageStoreSnapshot snapshot;
    {
        Table source{"Employees",
                     {{"id", ColumnType::Int}, {"name", ColumnType::String}}};
        ASSERT_TRUE(source.createIndex("idx_id", "id"));
        (void)source.insert({Value{static_cast<std::int64_t>(1)}, Value{std::string{"Alice"}}});
        (void)source.insert({Value{static_cast<std::int64_t>(2)}, Value{std::string{"Bob"}}});
        ASSERT_TRUE(source.erase(1));
        snapshot = source.exportPageStore();
        ASSERT_FALSE(snapshot.pages.empty());
    }

    {
        std::ofstream out{root / "legacy_v3.tcrdb", std::ios::binary};
        ASSERT_TRUE(out);

        const auto writePod = [&](const auto &value) {
            out.write(reinterpret_cast<const char *>(&value), sizeof(value));
        };
        const auto writeString = [&](std::string_view value) {
            writePod(static_cast<std::uint64_t>(value.size()));
            out.write(value.data(), static_cast<std::streamsize>(value.size()));
        };

        out.write("TCRDB001", 8);
        writePod(std::uint32_t{3}); // page-payload format without index pages
        writeString("legacy_v3");
        writePod(std::uint64_t{1}); // table count
        writeString("Employees");
        writePod(std::uint64_t{2}); // column count
        writeString("id");
        writePod(std::uint8_t{0}); // INT
        writePod(std::uint8_t{0}); // not nullable
        writeString("name");
        writePod(std::uint8_t{2}); // STRING
        writePod(std::uint8_t{0}); // not nullable
        writePod(std::uint64_t{1}); // one index (rebuilt on load)
        writeString("idx_id");
        writeString("id");

        writePod(static_cast<std::uint64_t>(snapshot.rowsPerPage));
        writePod(static_cast<std::uint64_t>(snapshot.capacity));
        writePod(static_cast<std::uint64_t>(snapshot.freeList.size()));
        for (const auto rowId : snapshot.freeList) {
            writePod(static_cast<std::uint64_t>(rowId));
        }
        writePod(static_cast<std::uint64_t>(snapshot.pages.size()));
        for (const auto &[pageId, bytes] : snapshot.pages) {
            writePod(static_cast<std::uint64_t>(pageId));
            writePod(static_cast<std::uint64_t>(bytes.size()));
            if (!bytes.empty()) {
                out.write(reinterpret_cast<const char *>(bytes.data()),
                          static_cast<std::streamsize>(bytes.size()));
            }
        }
    }

    StorageManager storage{root};
    auto database = storage.loadDatabase("legacy_v3");
    auto table = database->table("Employees");
    ASSERT_NE(table, nullptr);
    EXPECT_EQ(table->rowCount(), 1U);
    EXPECT_EQ(table->freeList(), snapshot.freeList);
    ASSERT_NE(table->getRow(0), std::nullopt);
    EXPECT_EQ((*table->getRow(0))[0], Value{static_cast<std::int64_t>(1)});
    EXPECT_EQ((*table->getRow(0))[1], Value{std::string{"Alice"}});
    EXPECT_EQ(table->getRow(1), std::nullopt);
    EXPECT_EQ(table->indexedLookup("id", Value{static_cast<std::int64_t>(1)})
                  .value_or(std::vector<RowId>{}),
              std::vector<RowId>{0});
    EXPECT_EQ(table->insert({Value{static_cast<std::int64_t>(3)}, Value{std::string{"Cara"}}}), 1U);

    std::filesystem::remove_all(root);
}

TEST(DeepFeatureTests, StorageManagerLoadsLegacyConstraintFlagsV5Snapshots) {
    // v5: UNIQUE / PRIMARY KEY column flags + page/index pages; no CHECK / FOREIGN KEY sections.
    const auto root =
        std::filesystem::temp_directory_path() /
        ("VertexDB_v5_snapshot_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);

    PageStoreSnapshot snapshot;
    {
        Table source{"Accounts",
                     {{"id", ColumnType::Int, false, true, true},
                      {"email", ColumnType::String, false, true, false}}};
        (void)source.insert({Value{static_cast<std::int64_t>(1)}, Value{std::string{"a@x"}}});
        snapshot = source.exportPageStore();
        ASSERT_FALSE(snapshot.pages.empty());
    }

    {
        std::ofstream out{root / "legacy_v5.tcrdb", std::ios::binary};
        ASSERT_TRUE(out);

        const auto writePod = [&](const auto &value) {
            out.write(reinterpret_cast<const char *>(&value), sizeof(value));
        };
        const auto writeString = [&](std::string_view value) {
            writePod(static_cast<std::uint64_t>(value.size()));
            out.write(value.data(), static_cast<std::streamsize>(value.size()));
        };

        out.write("TCRDB001", 8);
        writePod(std::uint32_t{5});
        writeString("legacy_v5");
        writePod(std::uint64_t{1}); // table count
        writeString("Accounts");
        writePod(std::uint64_t{2}); // column count
        writeString("id");
        writePod(std::uint8_t{0}); // INT
        writePod(std::uint8_t{0}); // not nullable
        writePod(std::uint8_t{1}); // unique
        writePod(std::uint8_t{1}); // primary key
        writeString("email");
        writePod(std::uint8_t{2}); // STRING
        writePod(std::uint8_t{0}); // not nullable
        writePod(std::uint8_t{1}); // unique
        writePod(std::uint8_t{0}); // not primary key
        // v5 has no CHECK / FOREIGN KEY sections.
        writePod(std::uint64_t{0}); // indexes (constraint indexes rebuilt on load)

        writePod(static_cast<std::uint64_t>(snapshot.rowsPerPage));
        writePod(static_cast<std::uint64_t>(snapshot.capacity));
        writePod(static_cast<std::uint64_t>(snapshot.freeList.size()));
        for (const auto rowId : snapshot.freeList) {
            writePod(static_cast<std::uint64_t>(rowId));
        }
        writePod(static_cast<std::uint64_t>(snapshot.pages.size()));
        for (const auto &[pageId, bytes] : snapshot.pages) {
            writePod(static_cast<std::uint64_t>(pageId));
            writePod(static_cast<std::uint64_t>(bytes.size()));
            if (!bytes.empty()) {
                out.write(reinterpret_cast<const char *>(bytes.data()),
                          static_cast<std::streamsize>(bytes.size()));
            }
        }
        writePod(std::uint64_t{0}); // index page count
    }

    StorageManager storage{root};
    auto database = storage.loadDatabase("legacy_v5");
    auto table = database->table("Accounts");
    ASSERT_NE(table, nullptr);
    ASSERT_EQ(table->schema().size(), 2U);
    EXPECT_TRUE(table->schema()[0].primaryKey);
    EXPECT_TRUE(table->schema()[0].unique);
    EXPECT_TRUE(table->schema()[1].unique);
    EXPECT_FALSE(table->schema()[1].primaryKey);
    EXPECT_TRUE(table->hasIndex("id"));
    EXPECT_TRUE(table->hasIndex("email"));
    EXPECT_EQ(table->rowCount(), 1U);
    EXPECT_THROW(table->assertUniqueRow(
                     {Value{static_cast<std::int64_t>(2)}, Value{std::string{"a@x"}}}),
                 std::invalid_argument);

    std::filesystem::remove_all(root);
}

TEST(DeepFeatureTests, StorageManagerLoadsLegacyCheckConstraintV6Snapshots) {
    // v6: CHECK predicate text after column flags; no FOREIGN KEY section.
    const auto root =
        std::filesystem::temp_directory_path() /
        ("VertexDB_v6_snapshot_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);

    PageStoreSnapshot snapshot;
    {
        Table source{"Pay",
                     {{"id", ColumnType::Int, false, true, true},
                      {"salary", ColumnType::Double}},
                     {}};
        (void)source.insert({Value{static_cast<std::int64_t>(1)}, Value{120000.0}});
        snapshot = source.exportPageStore();
        ASSERT_FALSE(snapshot.pages.empty());
    }

    {
        std::ofstream out{root / "legacy_v6.tcrdb", std::ios::binary};
        ASSERT_TRUE(out);

        const auto writePod = [&](const auto &value) {
            out.write(reinterpret_cast<const char *>(&value), sizeof(value));
        };
        const auto writeString = [&](std::string_view value) {
            writePod(static_cast<std::uint64_t>(value.size()));
            out.write(value.data(), static_cast<std::streamsize>(value.size()));
        };

        out.write("TCRDB001", 8);
        writePod(std::uint32_t{6});
        writeString("legacy_v6");
        writePod(std::uint64_t{1}); // table count
        writeString("Pay");
        writePod(std::uint64_t{2}); // column count
        writeString("id");
        writePod(std::uint8_t{0}); // INT
        writePod(std::uint8_t{0}); // not nullable
        writePod(std::uint8_t{1}); // unique
        writePod(std::uint8_t{1}); // primary key
        writeString("salary");
        writePod(std::uint8_t{1}); // DOUBLE
        writePod(std::uint8_t{0}); // not nullable
        writePod(std::uint8_t{0}); // not unique
        writePod(std::uint8_t{0}); // not primary key
        writePod(std::uint64_t{1}); // one CHECK
        writeString("salary > 0.0");
        // v6 has no FOREIGN KEY section.
        writePod(std::uint64_t{0}); // indexes

        writePod(static_cast<std::uint64_t>(snapshot.rowsPerPage));
        writePod(static_cast<std::uint64_t>(snapshot.capacity));
        writePod(static_cast<std::uint64_t>(snapshot.freeList.size()));
        for (const auto rowId : snapshot.freeList) {
            writePod(static_cast<std::uint64_t>(rowId));
        }
        writePod(static_cast<std::uint64_t>(snapshot.pages.size()));
        for (const auto &[pageId, bytes] : snapshot.pages) {
            writePod(static_cast<std::uint64_t>(pageId));
            writePod(static_cast<std::uint64_t>(bytes.size()));
            if (!bytes.empty()) {
                out.write(reinterpret_cast<const char *>(bytes.data()),
                          static_cast<std::streamsize>(bytes.size()));
            }
        }
        writePod(std::uint64_t{0}); // index page count
    }

    StorageManager storage{root};
    auto database = storage.loadDatabase("legacy_v6");
    auto table = database->table("Pay");
    ASSERT_NE(table, nullptr);
    ASSERT_EQ(table->checkConstraints().size(), 1U);
    EXPECT_TRUE(table->schema()[0].primaryKey);
    EXPECT_EQ(table->rowCount(), 1U);
    EXPECT_THROW(table->validateRow({Value{static_cast<std::int64_t>(2)}, Value{-1.0}}),
                 std::invalid_argument);

    std::filesystem::remove_all(root);
}

TEST(DeepFeatureTests, StorageManagerLoadsLegacyForeignKeyV7Snapshots) {
    // v7: single-column FK strings after CHECK; no composite UNIQUE section / multi-column FK lists.
    const auto root =
        std::filesystem::temp_directory_path() /
        ("VertexDB_v7_snapshot_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);

    PageStoreSnapshot customersSnapshot;
    PageStoreSnapshot ordersSnapshot;
    {
        Table customers{"Customers", {{"id", ColumnType::Int, false, true, true}}};
        (void)customers.insert({Value{static_cast<std::int64_t>(1)}});
        customersSnapshot = customers.exportPageStore();
        ASSERT_FALSE(customersSnapshot.pages.empty());

        Table orders{"Orders",
                     {{"id", ColumnType::Int, false, true, true},
                      {"customer_id", ColumnType::Int, false, false, false}}};
        (void)orders.insert({Value{static_cast<std::int64_t>(10)}, Value{static_cast<std::int64_t>(1)}});
        ordersSnapshot = orders.exportPageStore();
        ASSERT_FALSE(ordersSnapshot.pages.empty());
    }

    {
        std::ofstream out{root / "legacy_v7.tcrdb", std::ios::binary};
        ASSERT_TRUE(out);

        const auto writePod = [&](const auto &value) {
            out.write(reinterpret_cast<const char *>(&value), sizeof(value));
        };
        const auto writeString = [&](std::string_view value) {
            writePod(static_cast<std::uint64_t>(value.size()));
            out.write(value.data(), static_cast<std::streamsize>(value.size()));
        };
        const auto writePagePayload = [&](const PageStoreSnapshot &snapshot) {
            writePod(static_cast<std::uint64_t>(snapshot.rowsPerPage));
            writePod(static_cast<std::uint64_t>(snapshot.capacity));
            writePod(static_cast<std::uint64_t>(snapshot.freeList.size()));
            for (const auto rowId : snapshot.freeList) {
                writePod(static_cast<std::uint64_t>(rowId));
            }
            writePod(static_cast<std::uint64_t>(snapshot.pages.size()));
            for (const auto &[pageId, bytes] : snapshot.pages) {
                writePod(static_cast<std::uint64_t>(pageId));
                writePod(static_cast<std::uint64_t>(bytes.size()));
                if (!bytes.empty()) {
                    out.write(reinterpret_cast<const char *>(bytes.data()),
                              static_cast<std::streamsize>(bytes.size()));
                }
            }
            writePod(std::uint64_t{0}); // index page count
        };

        out.write("TCRDB001", 8);
        writePod(std::uint32_t{7});
        writeString("legacy_v7");
        writePod(std::uint64_t{2}); // table count

        writeString("Customers");
        writePod(std::uint64_t{1});
        writeString("id");
        writePod(std::uint8_t{0}); // INT
        writePod(std::uint8_t{0}); // not nullable
        writePod(std::uint8_t{1}); // unique
        writePod(std::uint8_t{1}); // primary key
        writePod(std::uint64_t{0}); // CHECK
        writePod(std::uint64_t{0}); // FK
        writePod(std::uint64_t{0}); // indexes
        writePagePayload(customersSnapshot);

        writeString("Orders");
        writePod(std::uint64_t{2});
        writeString("id");
        writePod(std::uint8_t{0});
        writePod(std::uint8_t{0});
        writePod(std::uint8_t{1});
        writePod(std::uint8_t{1});
        writeString("customer_id");
        writePod(std::uint8_t{0});
        writePod(std::uint8_t{0});
        writePod(std::uint8_t{0});
        writePod(std::uint8_t{0});
        writePod(std::uint64_t{0}); // CHECK
        writePod(std::uint64_t{1}); // one FK (width-1 strings)
        writeString("customer_id");
        writeString("Customers");
        writeString("id");
        writePod(std::uint8_t{0}); // NO ACTION delete
        writePod(std::uint8_t{0}); // NO ACTION update
        writePod(std::uint64_t{0}); // indexes
        writePagePayload(ordersSnapshot);
    }

    StorageManager storage{root};
    auto database = storage.loadDatabase("legacy_v7");
    auto orders = database->table("Orders");
    ASSERT_NE(orders, nullptr);
    ASSERT_EQ(orders->foreignKeys().size(), 1U);
    EXPECT_EQ(orders->foreignKeys()[0].childColumns, (std::vector<std::string>{"customer_id"}));
    EXPECT_EQ(orders->foreignKeys()[0].parentTable, "Customers");
    EXPECT_EQ(orders->foreignKeys()[0].parentColumns, (std::vector<std::string>{"id"}));
    EXPECT_EQ(orders->foreignKeys()[0].onDelete, ForeignKeyAction::NoAction);
    EXPECT_EQ(orders->rowCount(), 1U);
    auto customers = database->table("Customers");
    ASSERT_NE(customers, nullptr);
    EXPECT_EQ(customers->rowCount(), 1U);
    EXPECT_TRUE(customers->schema()[0].primaryKey);

    std::filesystem::remove_all(root);
}

TEST(DeepFeatureTests, StorageManagerLoadsLegacyCompositeUniqueV8Snapshots) {
    // v8: table-level composite UNIQUE after CHECK/FK; still width-1 FK encoding (not v9 lists).
    const auto root =
        std::filesystem::temp_directory_path() /
        ("VertexDB_v8_snapshot_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);

    PageStoreSnapshot snapshot;
    {
        Table source{"Tags",
                     {{"id", ColumnType::Int, false, true, true},
                      {"a", ColumnType::Int, true, false, false},
                      {"b", ColumnType::Int, true, false, false}}};
        (void)source.insert({Value{static_cast<std::int64_t>(1)}, Value{static_cast<std::int64_t>(10)},
                             Value{static_cast<std::int64_t>(20)}});
        snapshot = source.exportPageStore();
        ASSERT_FALSE(snapshot.pages.empty());
    }

    {
        std::ofstream out{root / "legacy_v8.tcrdb", std::ios::binary};
        ASSERT_TRUE(out);

        const auto writePod = [&](const auto &value) {
            out.write(reinterpret_cast<const char *>(&value), sizeof(value));
        };
        const auto writeString = [&](std::string_view value) {
            writePod(static_cast<std::uint64_t>(value.size()));
            out.write(value.data(), static_cast<std::streamsize>(value.size()));
        };

        out.write("TCRDB001", 8);
        writePod(std::uint32_t{8});
        writeString("legacy_v8");
        writePod(std::uint64_t{1});
        writeString("Tags");
        writePod(std::uint64_t{3});
        writeString("id");
        writePod(std::uint8_t{0});
        writePod(std::uint8_t{0});
        writePod(std::uint8_t{1});
        writePod(std::uint8_t{1});
        writeString("a");
        writePod(std::uint8_t{0});
        writePod(std::uint8_t{1}); // nullable
        writePod(std::uint8_t{0});
        writePod(std::uint8_t{0});
        writeString("b");
        writePod(std::uint8_t{0});
        writePod(std::uint8_t{1});
        writePod(std::uint8_t{0});
        writePod(std::uint8_t{0});
        writePod(std::uint64_t{0}); // CHECK
        writePod(std::uint64_t{0}); // FK
        writePod(std::uint64_t{1}); // one composite UNIQUE
        writePod(std::uint8_t{0});  // not primaryKey
        writePod(std::uint64_t{2});
        writeString("a");
        writeString("b");
        writePod(std::uint64_t{0}); // indexes (constraint indexes rebuilt on load)

        writePod(static_cast<std::uint64_t>(snapshot.rowsPerPage));
        writePod(static_cast<std::uint64_t>(snapshot.capacity));
        writePod(static_cast<std::uint64_t>(snapshot.freeList.size()));
        for (const auto rowId : snapshot.freeList) {
            writePod(static_cast<std::uint64_t>(rowId));
        }
        writePod(static_cast<std::uint64_t>(snapshot.pages.size()));
        for (const auto &[pageId, bytes] : snapshot.pages) {
            writePod(static_cast<std::uint64_t>(pageId));
            writePod(static_cast<std::uint64_t>(bytes.size()));
            if (!bytes.empty()) {
                out.write(reinterpret_cast<const char *>(bytes.data()),
                          static_cast<std::streamsize>(bytes.size()));
            }
        }
        writePod(std::uint64_t{0}); // index page count
    }

    StorageManager storage{root};
    auto database = storage.loadDatabase("legacy_v8");
    auto table = database->table("Tags");
    ASSERT_NE(table, nullptr);
    ASSERT_EQ(table->uniqueConstraints().size(), 1U);
    EXPECT_FALSE(table->uniqueConstraints()[0].primaryKey);
    EXPECT_EQ(table->uniqueConstraints()[0].columns, (std::vector<std::string>{"a", "b"}));
    EXPECT_EQ(table->rowCount(), 1U);
    EXPECT_THROW(table->assertUniqueRow({Value{static_cast<std::int64_t>(2)},
                                         Value{static_cast<std::int64_t>(10)},
                                         Value{static_cast<std::int64_t>(20)}}),
                 std::invalid_argument);

    std::filesystem::remove_all(root);
}

} // namespace VertexDB
