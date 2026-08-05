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
}

void BTreeIndex::insert(const Value &key, RowId rowId) {
    std::unique_lock lock{mutex_};
    entries_[key].push_back(rowId);
    rebuildLayout();
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
    rebuildLayout();
}

void BTreeIndex::clear() {
    std::unique_lock lock{mutex_};
    entries_.clear();
    rebuildLayout();
}

std::vector<RowId> BTreeIndex::find(const Value &key) const {
    std::shared_lock lock{mutex_};
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
    std::shared_lock lock{mutex_};
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
    std::shared_lock lock{mutex_};
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
    std::shared_lock lock{mutex_};
    return nodes_.size() <= 1 ? 1 : 2;
}

std::size_t BTreeIndex::leafPageCount() const {
    std::shared_lock lock{mutex_};
    return static_cast<std::size_t>(
        std::ranges::count_if(nodes_, [](const BTreeNode &node) { return node.leaf; }));
}

std::vector<BTreeNode> BTreeIndex::nodesSnapshot() const {
    std::shared_lock lock{mutex_};
    return nodes_;
}

void BTreeIndex::rebuildLayout() {
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
