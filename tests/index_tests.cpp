#include "VertexDB/indexing/btree_index.hpp"
#include "VertexDB/indexing/hash_index.hpp"
#include "VertexDB/common/value.hpp"

#include <gtest/gtest.h>

#include <optional>
#include <unordered_map>
#include <vector>

namespace VertexDB {



TEST(IndexTests, FindsInsertedRowIds) {
    HashIndex index;
    index.insert(Value{42}, 7);
    index.insert(Value{42}, 8);

    const auto rowIds = index.find(Value{42});
    ASSERT_EQ(rowIds.size(), 2U);
    EXPECT_EQ(rowIds[0], 7U);
    EXPECT_EQ(rowIds[1], 8U);
}

TEST(IndexTests, IncrementalBTreeSplitMergeMaintainsStructuralInvariants) {
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
