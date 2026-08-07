#include "VertexDB/indexing/btree_index.hpp"

#include "btree_index_detail.hpp"

#include <stdexcept>
#include <utility>

namespace VertexDB {

BTreeIndex::BTreeIndex(std::size_t maxKeysPerNode) : maxKeysPerNode_(maxKeysPerNode) {
    if (maxKeysPerNode_ < 2) {
        throw std::invalid_argument("B+ tree node capacity must be at least 2");
    }
    BTreeNode root;
    root.pageId = rootPageId_;
    root.leaf = true;
    nodes_.emplace(rootPageId_, std::move(root));
    clearDirtyPages();
}

BTreeNode &BTreeIndex::node(BTreePageId pageId) {
    markDirty(pageId);
    return nodes_.at(pageId);
}

const BTreeNode &BTreeIndex::node(BTreePageId pageId) const { return nodes_.at(pageId); }

std::size_t BTreeIndex::minKeys() const noexcept { return maxKeysPerNode_ / 2; }

std::size_t BTreeIndex::childIndex(const BTreeNode &internal, const Value &key) const {
    std::size_t index = 0;
    while (index < internal.keys.size() && !(key < internal.keys[index])) {
        ++index;
    }
    return index;
}

BTreePageId BTreeIndex::childFor(const BTreeNode &internal, const Value &key) const {
    return internal.children[childIndex(internal, key)];
}

BTreePageId BTreeIndex::leftmostLeaf() const {
    BTreePageId pageId = rootPageId_;
    while (!node(pageId).leaf) {
        pageId = node(pageId).children.front();
    }
    return pageId;
}

std::vector<BTreePageId> BTreeIndex::pathToLeaf(const Value &key) const {
    std::vector<BTreePageId> path;
    BTreePageId pageId = rootPageId_;
    while (true) {
        path.push_back(pageId);
        const auto &current = node(pageId);
        if (current.leaf) {
            break;
        }
        pageId = childFor(current, key);
    }
    return path;
}

std::vector<RowId> BTreeIndex::find(const Value &key) const {
    const auto leafId = pathToLeaf(key).back();
    const auto &leaf = node(leafId);
    const auto index = btree_detail::lowerBoundKey(leaf.keys, key);
    if (index < leaf.keys.size() && leaf.keys[index] == key) {
        return leaf.rowIds[index];
    }
    return {};
}

std::vector<RowId> BTreeIndex::lessThan(const Value &key) const {
    std::vector<RowId> result;
    std::optional<BTreePageId> pageId = leftmostLeaf();
    while (pageId) {
        const auto &leaf = node(*pageId);
        for (std::size_t i = 0; i < leaf.keys.size(); ++i) {
            if (!(leaf.keys[i] < key)) {
                return result;
            }
            result.insert(result.end(), leaf.rowIds[i].begin(), leaf.rowIds[i].end());
        }
        pageId = leaf.nextLeaf;
    }
    return result;
}

std::vector<RowId> BTreeIndex::greaterThan(const Value &key) const {
    std::vector<RowId> result;
    const auto startId = pathToLeaf(key).back();
    std::optional<BTreePageId> pageId = startId;
    bool collecting = false;
    while (pageId) {
        const auto &leaf = node(*pageId);
        for (std::size_t i = 0; i < leaf.keys.size(); ++i) {
            if (!collecting && key < leaf.keys[i]) {
                collecting = true;
            }
            if (collecting) {
                result.insert(result.end(), leaf.rowIds[i].begin(), leaf.rowIds[i].end());
            }
        }
        pageId = leaf.nextLeaf;
    }
    return result;
}

std::size_t BTreeIndex::size() const { return keyCount_; }

std::size_t BTreeIndex::height() const {
    std::size_t levels = 1;
    BTreePageId pageId = rootPageId_;
    while (!node(pageId).leaf) {
        ++levels;
        pageId = node(pageId).children.front();
    }
    return levels;
}

std::size_t BTreeIndex::leafPageCount() const {
    std::size_t count = 0;
    std::optional<BTreePageId> pageId = leftmostLeaf();
    while (pageId) {
        ++count;
        pageId = node(*pageId).nextLeaf;
    }
    return count;
}

} // namespace VertexDB
