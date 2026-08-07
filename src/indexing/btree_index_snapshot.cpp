#include "VertexDB/indexing/btree_index.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace VertexDB {

std::vector<BTreeNode> BTreeIndex::nodesSnapshot() const {
    std::vector<BTreeNode> snapshot;
    snapshot.reserve(nodes_.size());

    std::optional<BTreePageId> leafId = leftmostLeaf();
    while (leafId) {
        snapshot.push_back(node(*leafId));
        leafId = node(*leafId).nextLeaf;
    }

    for (const auto &[pageId, page] : nodes_) {
        if (!page.leaf) {
            snapshot.push_back(page);
        }
    }

    // Keep the root last when it is an internal node so existing layout checks stay readable.
    if (!node(rootPageId_).leaf) {
        auto rootIt = std::find_if(snapshot.begin(), snapshot.end(),
                                   [this](const BTreeNode &page) {
                                       return !page.leaf && page.pageId == rootPageId_;
                                   });
        if (rootIt != snapshot.end() && std::next(rootIt) != snapshot.end()) {
            auto rootNode = std::move(*rootIt);
            snapshot.erase(rootIt);
            snapshot.push_back(std::move(rootNode));
        }
    }
    return snapshot;
}

BTreeIndexSnapshot BTreeIndex::exportPages() const {
    BTreeIndexSnapshot snapshot;
    snapshot.maxKeysPerNode = maxKeysPerNode_;
    snapshot.rootPageId = rootPageId_;
    snapshot.nextPageId = nextPageId_;
    snapshot.freePageIds = freePageIds_;
    snapshot.keyCount = keyCount_;
    snapshot.nodes.reserve(nodes_.size());
    for (const auto &[_, page] : nodes_) {
        snapshot.nodes.push_back(page);
    }
    std::sort(snapshot.nodes.begin(), snapshot.nodes.end(),
              [](const BTreeNode &lhs, const BTreeNode &rhs) { return lhs.pageId < rhs.pageId; });
    return snapshot;
}

void BTreeIndex::replaceFromPages(BTreeIndexSnapshot snapshot) {
    if (snapshot.maxKeysPerNode < 2) {
        throw std::invalid_argument("B+ tree node capacity must be at least 2");
    }
    maxKeysPerNode_ = snapshot.maxKeysPerNode;
    rootPageId_ = snapshot.rootPageId;
    nextPageId_ = snapshot.nextPageId;
    freePageIds_ = std::move(snapshot.freePageIds);
    keyCount_ = snapshot.keyCount;
    nodes_.clear();
    for (auto &page : snapshot.nodes) {
        const auto pageId = page.pageId;
        nodes_.emplace(pageId, std::move(page));
    }
    if (!nodes_.contains(rootPageId_)) {
        throw std::invalid_argument("B+ tree snapshot is missing the root page");
    }
    clearDirtyPages();
}

void BTreeIndex::clearDirtyPages() noexcept {
    dirtyPages_.clear();
    metadataDirty_ = false;
    fullReplaceDirty_ = false;
}

bool BTreeIndex::hasDirtyPages() const noexcept {
    return fullReplaceDirty_ || metadataDirty_ || !dirtyPages_.empty();
}

BTreeIndexSnapshot BTreeIndex::takeDirtyPages() {
    if (!hasDirtyPages()) {
        return {};
    }
    // Always ship a complete live node set so redo can install without merging frees.
    auto snapshot = exportPages();
    clearDirtyPages();
    return snapshot;
}

void BTreeIndex::applyDirtyPages(const BTreeIndexSnapshot &dirty) {
    if (dirty.nodes.empty() && dirty.keyCount == 0 && dirty.rootPageId == 0) {
        return;
    }
    replaceFromPages(dirty);
}

} // namespace VertexDB
