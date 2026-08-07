#include "VertexDB/indexing/btree_index.hpp"

#include "btree_index_detail.hpp"

#include <stdexcept>
#include <utility>

namespace VertexDB {

void BTreeIndex::markDirty(BTreePageId pageId) {
    dirtyPages_[pageId] = true;
    metadataDirty_ = true;
}

BTreePageId BTreeIndex::allocatePage() {
    metadataDirty_ = true;
    if (!freePageIds_.empty()) {
        const auto id = freePageIds_.back();
        freePageIds_.pop_back();
        return id;
    }
    return nextPageId_++;
}

void BTreeIndex::freePage(BTreePageId pageId) {
    nodes_.erase(pageId);
    freePageIds_.push_back(pageId);
    dirtyPages_.erase(pageId);
    metadataDirty_ = true;
    fullReplaceDirty_ = true; // freed pages require a full node set on redo
}

void BTreeIndex::insert(const Value &key, RowId rowId) {
    auto path = pathToLeaf(key);
    insertIntoLeaf(path, key, rowId);
}

void BTreeIndex::insertIntoLeaf(std::vector<BTreePageId> &path, const Value &key, RowId rowId) {
    auto &leaf = node(path.back());
    const auto index = btree_detail::lowerBoundKey(leaf.keys, key);
    if (index < leaf.keys.size() && leaf.keys[index] == key) {
        leaf.rowIds[index].push_back(rowId);
        return;
    }

    leaf.keys.insert(leaf.keys.begin() + static_cast<std::ptrdiff_t>(index), key);
    leaf.rowIds.insert(leaf.rowIds.begin() + static_cast<std::ptrdiff_t>(index),
                       std::vector<RowId>{rowId});
    ++keyCount_;

    if (leaf.keys.size() > maxKeysPerNode_) {
        splitLeaf(path);
    }
}

void BTreeIndex::splitLeaf(std::vector<BTreePageId> &path) {
    const auto leftId = path.back();
    auto &left = node(leftId);
    const std::size_t mid = left.keys.size() / 2;

    BTreeNode right;
    right.pageId = allocatePage();
    right.leaf = true;
    right.keys.assign(left.keys.begin() + static_cast<std::ptrdiff_t>(mid), left.keys.end());
    right.rowIds.assign(left.rowIds.begin() + static_cast<std::ptrdiff_t>(mid), left.rowIds.end());
    right.nextLeaf = left.nextLeaf;

    left.keys.resize(mid);
    left.rowIds.resize(mid);
    left.nextLeaf = right.pageId;

    const Value separator = right.keys.front();
    const auto rightId = right.pageId;
    nodes_.emplace(rightId, std::move(right));

    path.pop_back();
    insertSeparator(path, separator, rightId);
}

void BTreeIndex::insertSeparator(std::vector<BTreePageId> &path, const Value &separator,
                                 BTreePageId rightChild) {
    if (path.empty()) {
        BTreeNode newRoot;
        newRoot.pageId = allocatePage();
        newRoot.leaf = false;
        newRoot.keys.push_back(separator);
        newRoot.children.push_back(rootPageId_);
        newRoot.children.push_back(rightChild);
        rootPageId_ = newRoot.pageId;
        nodes_.emplace(rootPageId_, std::move(newRoot));
        return;
    }

    auto &parent = node(path.back());
    const auto index = childIndex(parent, separator);
    parent.keys.insert(parent.keys.begin() + static_cast<std::ptrdiff_t>(index), separator);
    parent.children.insert(parent.children.begin() + static_cast<std::ptrdiff_t>(index + 1),
                           rightChild);

    if (parent.keys.size() > maxKeysPerNode_) {
        splitInternal(path);
    }
}

void BTreeIndex::splitInternal(std::vector<BTreePageId> &path) {
    const auto leftId = path.back();
    auto &left = node(leftId);
    const std::size_t mid = left.keys.size() / 2;
    const Value promoted = left.keys[mid];

    BTreeNode right;
    right.pageId = allocatePage();
    right.leaf = false;
    right.keys.assign(left.keys.begin() + static_cast<std::ptrdiff_t>(mid + 1), left.keys.end());
    right.children.assign(left.children.begin() + static_cast<std::ptrdiff_t>(mid + 1),
                          left.children.end());

    left.keys.resize(mid);
    left.children.resize(mid + 1);

    const auto rightId = right.pageId;
    nodes_.emplace(rightId, std::move(right));

    path.pop_back();
    insertSeparator(path, promoted, rightId);
}

void BTreeIndex::remove(const Value &key, RowId rowId) {
    auto path = pathToLeaf(key);
    removeFromLeaf(path, key, rowId);
}

void BTreeIndex::removeFromLeaf(std::vector<BTreePageId> &path, const Value &key, RowId rowId) {
    auto &leaf = node(path.back());
    const auto index = btree_detail::lowerBoundKey(leaf.keys, key);
    if (index >= leaf.keys.size() || !(leaf.keys[index] == key)) {
        return;
    }

    std::erase(leaf.rowIds[index], rowId);
    if (!leaf.rowIds[index].empty()) {
        return;
    }

    leaf.keys.erase(leaf.keys.begin() + static_cast<std::ptrdiff_t>(index));
    leaf.rowIds.erase(leaf.rowIds.begin() + static_cast<std::ptrdiff_t>(index));
    --keyCount_;

    if (path.size() == 1) {
        return;
    }

    if (index == 0 && !leaf.keys.empty()) {
        const auto parentId = path[path.size() - 2];
        const auto parentChild = childIndex(node(parentId), key);
        if (parentChild > 0) {
            updateSeparator(parentId, parentChild, leaf.keys.front());
        }
    }

    if (leaf.keys.size() < minKeys()) {
        rebalanceAfterDelete(path);
    }
}

void BTreeIndex::updateSeparator(BTreePageId parentId, std::size_t childIndex,
                                 const Value &separator) {
    auto &parent = node(parentId);
    if (childIndex == 0 || childIndex > parent.keys.size()) {
        return;
    }
    parent.keys[childIndex - 1] = separator;
}

void BTreeIndex::rebalanceAfterDelete(std::vector<BTreePageId> &path) {
    while (path.size() > 1) {
        const auto pageId = path.back();
        const auto parentId = path[path.size() - 2];
        auto &current = node(pageId);
        auto &parent = node(parentId);

        std::size_t index = 0;
        while (index < parent.children.size() && parent.children[index] != pageId) {
            ++index;
        }
        if (index >= parent.children.size()) {
            throw std::logic_error("B+ tree child missing from parent");
        }

        const bool isLeaf = current.leaf;
        const bool underfull =
            isLeaf ? current.keys.size() < minKeys() : current.keys.size() < minKeys();
        if (!underfull) {
            break;
        }

        if (index > 0) {
            auto &left = node(parent.children[index - 1]);
            if (left.keys.size() > minKeys()) {
                if (isLeaf) {
                    borrowFromLeftLeaf(parentId, index);
                } else {
                    borrowFromLeftInternal(parentId, index);
                }
                break;
            }
        }
        if (index + 1 < parent.children.size()) {
            auto &right = node(parent.children[index + 1]);
            if (right.keys.size() > minKeys()) {
                if (isLeaf) {
                    borrowFromRightLeaf(parentId, index);
                } else {
                    borrowFromRightInternal(parentId, index);
                }
                break;
            }
        }

        if (index > 0) {
            if (isLeaf) {
                mergeLeaves(parentId, index - 1);
            } else {
                mergeInternals(parentId, index - 1);
            }
        } else {
            if (isLeaf) {
                mergeLeaves(parentId, index);
            } else {
                mergeInternals(parentId, index);
            }
        }

        path.pop_back();
        if (node(parentId).keys.size() >= minKeys() || path.size() == 1) {
            break;
        }
    }

    collapseRootIfNeeded();
}

void BTreeIndex::borrowFromLeftLeaf(BTreePageId parentId, std::size_t childIndex) {
    auto &parent = node(parentId);
    auto &left = node(parent.children[childIndex - 1]);
    auto &right = node(parent.children[childIndex]);

    right.keys.insert(right.keys.begin(), left.keys.back());
    right.rowIds.insert(right.rowIds.begin(), std::move(left.rowIds.back()));
    left.keys.pop_back();
    left.rowIds.pop_back();
    parent.keys[childIndex - 1] = right.keys.front();
}

void BTreeIndex::borrowFromRightLeaf(BTreePageId parentId, std::size_t childIndex) {
    auto &parent = node(parentId);
    auto &left = node(parent.children[childIndex]);
    auto &right = node(parent.children[childIndex + 1]);

    left.keys.push_back(right.keys.front());
    left.rowIds.push_back(std::move(right.rowIds.front()));
    right.keys.erase(right.keys.begin());
    right.rowIds.erase(right.rowIds.begin());
    parent.keys[childIndex] = right.keys.front();
}

void BTreeIndex::borrowFromLeftInternal(BTreePageId parentId, std::size_t childIndex) {
    auto &parent = node(parentId);
    auto &left = node(parent.children[childIndex - 1]);
    auto &right = node(parent.children[childIndex]);

    right.keys.insert(right.keys.begin(), parent.keys[childIndex - 1]);
    right.children.insert(right.children.begin(), left.children.back());
    parent.keys[childIndex - 1] = left.keys.back();
    left.keys.pop_back();
    left.children.pop_back();
}

void BTreeIndex::borrowFromRightInternal(BTreePageId parentId, std::size_t childIndex) {
    auto &parent = node(parentId);
    auto &left = node(parent.children[childIndex]);
    auto &right = node(parent.children[childIndex + 1]);

    left.keys.push_back(parent.keys[childIndex]);
    left.children.push_back(right.children.front());
    parent.keys[childIndex] = right.keys.front();
    right.keys.erase(right.keys.begin());
    right.children.erase(right.children.begin());
}

void BTreeIndex::mergeLeaves(BTreePageId parentId, std::size_t leftChildIndex) {
    auto &parent = node(parentId);
    const auto leftId = parent.children[leftChildIndex];
    const auto rightId = parent.children[leftChildIndex + 1];
    auto &left = node(leftId);
    auto &right = node(rightId);

    left.keys.insert(left.keys.end(), right.keys.begin(), right.keys.end());
    left.rowIds.insert(left.rowIds.end(), right.rowIds.begin(), right.rowIds.end());
    left.nextLeaf = right.nextLeaf;

    parent.keys.erase(parent.keys.begin() + static_cast<std::ptrdiff_t>(leftChildIndex));
    parent.children.erase(parent.children.begin() +
                          static_cast<std::ptrdiff_t>(leftChildIndex + 1));
    freePage(rightId);
}

void BTreeIndex::mergeInternals(BTreePageId parentId, std::size_t leftChildIndex) {
    auto &parent = node(parentId);
    const auto leftId = parent.children[leftChildIndex];
    const auto rightId = parent.children[leftChildIndex + 1];
    auto &left = node(leftId);
    auto &right = node(rightId);

    left.keys.push_back(parent.keys[leftChildIndex]);
    left.keys.insert(left.keys.end(), right.keys.begin(), right.keys.end());
    left.children.insert(left.children.end(), right.children.begin(), right.children.end());

    parent.keys.erase(parent.keys.begin() + static_cast<std::ptrdiff_t>(leftChildIndex));
    parent.children.erase(parent.children.begin() +
                          static_cast<std::ptrdiff_t>(leftChildIndex + 1));
    freePage(rightId);
}

void BTreeIndex::collapseRootIfNeeded() {
    auto &root = node(rootPageId_);
    if (root.leaf) {
        return;
    }
    if (root.children.size() != 1) {
        return;
    }
    const auto onlyChild = root.children.front();
    freePage(rootPageId_);
    rootPageId_ = onlyChild;
}

void BTreeIndex::clear() {
    nodes_.clear();
    freePageIds_.clear();
    nextPageId_ = 2;
    rootPageId_ = 1;
    keyCount_ = 0;
    BTreeNode root;
    root.pageId = rootPageId_;
    root.leaf = true;
    nodes_.emplace(rootPageId_, std::move(root));
    dirtyPages_.clear();
    fullReplaceDirty_ = true;
    metadataDirty_ = true;
}

} // namespace VertexDB
