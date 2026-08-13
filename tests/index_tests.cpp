#include "VertexDB/indexing/btree_index.hpp"
#include "VertexDB/indexing/hash_index.hpp"
#include "VertexDB/common/index_expression.hpp"
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

TEST(IndexTests, CompositeValueKeysHashAndCompare) {
    HashIndex index;
    const auto key = Value::composite({Value{1}, Value{std::string{"a"}}});
    const auto same = Value::composite({Value{1}, Value{std::string{"a"}}});
    const auto other = Value::composite({Value{1}, Value{std::string{"b"}}});
    index.insert(key, 9);
    EXPECT_EQ(index.find(same).size(), 1U);
    EXPECT_TRUE(index.find(other).empty());
    EXPECT_TRUE(key == same);
    EXPECT_TRUE(key < other);

    BTreeIndex ordered{4};
    ordered.insert(key, 9);
    ordered.insert(other, 10);
    EXPECT_EQ(ordered.find(same).size(), 1U);
    EXPECT_EQ(ordered.greaterThan(key).size(), 1U);
}

TEST(IndexTests, CompositeIndexDefinitionEncodingRoundTrips) {
    const auto encoded = encodeIndexDefinitionColumns({"a", "b"}, std::nullopt);
    EXPECT_EQ(encoded, "cols:a,b");
    const auto decoded = decodeIndexDefinitionColumns(encoded);
    ASSERT_EQ(decoded.first.size(), 2U);
    EXPECT_EQ(decoded.first[0], "a");
    EXPECT_EQ(decoded.first[1], "b");
    EXPECT_FALSE(decoded.second.has_value());
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

TEST(IndexTests, ParseIndexExpressionStringRoundTripsSupportedShapes) {
    const auto column = parseIndexExpressionString("salary");
    ASSERT_TRUE(column);
    EXPECT_EQ(column->kind, IndexExpression::Kind::Column);
    EXPECT_EQ(column->column, "salary");
    EXPECT_EQ(indexExpressionToString(*column), "salary");

    const auto negate = parseIndexExpressionString("-salary");
    ASSERT_TRUE(negate);
    EXPECT_EQ(negate->kind, IndexExpression::Kind::Negate);
    EXPECT_EQ(indexExpressionToString(*negate), "-salary");

    const auto add = parseIndexExpressionString("id+1");
    ASSERT_TRUE(add);
    EXPECT_EQ(add->kind, IndexExpression::Kind::Add);
    EXPECT_EQ(add->literal, Value{std::int64_t{1}});
    EXPECT_EQ(indexExpressionToString(*add), "id+1");

    const auto sub = parseIndexExpressionString("id-2.5");
    ASSERT_TRUE(sub);
    EXPECT_EQ(sub->kind, IndexExpression::Kind::Subtract);
    EXPECT_EQ(sub->literal, Value{2.5});
    EXPECT_EQ(indexExpressionToString(*sub), "id-2.5");

    const auto encoded = encodeIndexDefinitionColumn("id", add);
    EXPECT_EQ(encoded, "expr:id+1");
    const auto decoded = decodeIndexDefinitionColumn(encoded);
    ASSERT_TRUE(decoded.second);
    EXPECT_EQ(decoded.first, "id");
    EXPECT_EQ(*decoded.second, *add);

    EXPECT_FALSE(parseIndexExpressionString(""));
    EXPECT_FALSE(parseIndexExpressionString("a.b"));
    EXPECT_FALSE(parseIndexExpressionString("id*2"));

    const auto trigram = parseIndexExpressionString("trigram(name)");
    ASSERT_TRUE(trigram);
    EXPECT_EQ(trigram->kind, IndexExpression::Kind::Trigram);
    EXPECT_EQ(trigram->column, "name");
    EXPECT_EQ(indexExpressionToString(*trigram), "trigram(name)");
}

} // namespace VertexDB
