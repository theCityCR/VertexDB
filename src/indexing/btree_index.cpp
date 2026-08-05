#include "VertexDB/indexing/btree_index.hpp"

#include <algorithm>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>

namespace VertexDB {

BTreeIndex::BTreeIndex(std::size_t maxKeysPerLeaf) : maxKeysPerLeaf_(maxKeysPerLeaf) {
    if (maxKeysPerLeaf_ == 0) {
        throw std::invalid_argument("B+ tree leaf capacity must be positive");
    }
    rebuildLayout();
    layoutDirty_ = false;
}

void BTreeIndex::insert(const Value &key, RowId rowId) {
    std::unique_lock lock{mutex_};
    entries_[key].push_back(rowId);
    layoutDirty_ = true;
}

void BTreeIndex::remove(const Value &key, RowId rowId) {
    std::unique_lock lock{mutex_};
    auto it = entries_.find(key);
    if (it == entries_.end()) {
        return;
    }
    std::erase(it->second, rowId);
    if (it->second.empty()) {
        entries_.erase(it);
    }
    layoutDirty_ = true;
}

void BTreeIndex::clear() {
    std::unique_lock lock{mutex_};
    entries_.clear();
    layoutDirty_ = true;
}

std::vector<RowId> BTreeIndex::find(const Value &key) const {
    std::unique_lock lock{mutex_};
    ensureLayout();
    for (const auto &node : nodes_) {
        if (!node.leaf) {
            continue;
        }
        for (std::size_t i = 0; i < node.keys.size(); ++i) {
            if (node.keys[i] == key) {
                return node.rowIds[i];
            }
        }
    }
    return {};
}

std::vector<RowId> BTreeIndex::lessThan(const Value &key) const {
    std::unique_lock lock{mutex_};
    ensureLayout();
    std::vector<RowId> result;
    for (const auto &node : nodes_) {
        if (!node.leaf) {
            continue;
        }
        for (std::size_t i = 0; i < node.keys.size(); ++i) {
            if (!(node.keys[i] < key)) {
                return result;
            }
            result.insert(result.end(), node.rowIds[i].begin(), node.rowIds[i].end());
        }
    }
    return result;
}

std::vector<RowId> BTreeIndex::greaterThan(const Value &key) const {
    std::unique_lock lock{mutex_};
    ensureLayout();
    std::vector<RowId> result;
    bool collecting = false;
    for (const auto &node : nodes_) {
        if (!node.leaf) {
            continue;
        }
        for (std::size_t i = 0; i < node.keys.size(); ++i) {
            if (!collecting && key < node.keys[i]) {
                collecting = true;
            }
            if (collecting) {
                result.insert(result.end(), node.rowIds[i].begin(), node.rowIds[i].end());
            }
        }
    }
    return result;
}

std::size_t BTreeIndex::size() const {
    std::shared_lock lock{mutex_};
    return entries_.size();
}

std::size_t BTreeIndex::height() const {
    std::unique_lock lock{mutex_};
    ensureLayout();
    return nodes_.size() <= 1 ? 1 : 2;
}

std::size_t BTreeIndex::leafPageCount() const {
    std::unique_lock lock{mutex_};
    ensureLayout();
    return static_cast<std::size_t>(
        std::ranges::count_if(nodes_, [](const BTreeNode &node) { return node.leaf; }));
}

std::vector<BTreeNode> BTreeIndex::nodesSnapshot() const {
    std::unique_lock lock{mutex_};
    ensureLayout();
    return nodes_;
}

void BTreeIndex::ensureLayout() const {
    if (!layoutDirty_) {
        return;
    }
    rebuildLayout();
    layoutDirty_ = false;
}

void BTreeIndex::rebuildLayout() const {
    nodes_.clear();
    if (entries_.empty()) {
        nodes_.push_back(BTreeNode{1, true, {}, {}, {}, std::nullopt});
        return;
    }

    std::vector<BTreePageId> leafPageIds;
    BTreePageId nextPageId = 1;
    auto it = entries_.begin();
    while (it != entries_.end()) {
        BTreeNode leaf;
        leaf.pageId = nextPageId++;
        leaf.leaf = true;
        for (std::size_t count = 0; count < maxKeysPerLeaf_ && it != entries_.end();
             ++count, ++it) {
            leaf.keys.push_back(it->first);
            leaf.rowIds.push_back(it->second);
        }
        leafPageIds.push_back(leaf.pageId);
        nodes_.push_back(std::move(leaf));
    }

    for (std::size_t i = 0; i + 1 < nodes_.size(); ++i) {
        nodes_[i].nextLeaf = nodes_[i + 1].pageId;
    }

    if (nodes_.size() == 1) {
        return;
    }

    BTreeNode root;
    root.pageId = nextPageId;
    root.leaf = false;
    root.children = std::move(leafPageIds);
    for (std::size_t i = 1; i < nodes_.size(); ++i) {
        root.keys.push_back(nodes_[i].keys.front());
    }
    nodes_.push_back(std::move(root));
}

} // namespace VertexDB
