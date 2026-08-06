#pragma once

#include "VertexDB/storage/row.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace VertexDB {

using BTreePageId = std::uint64_t;

struct BTreeNode {
    BTreePageId pageId{};
    bool leaf{true};
    std::vector<Value> keys;
    std::vector<std::vector<RowId>> rowIds;
    std::vector<BTreePageId> children;
    std::optional<BTreePageId> nextLeaf;
};

class BTreeIndex {
  public:
    explicit BTreeIndex(std::size_t maxKeysPerNode = 64);

    void insert(const Value &key, RowId rowId);
    void remove(const Value &key, RowId rowId);
    void clear();

    [[nodiscard]] std::vector<RowId> find(const Value &key) const;
    [[nodiscard]] std::vector<RowId> lessThan(const Value &key) const;
    [[nodiscard]] std::vector<RowId> greaterThan(const Value &key) const;
    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] std::size_t height() const;
    [[nodiscard]] std::size_t leafPageCount() const;
    [[nodiscard]] std::vector<BTreeNode> nodesSnapshot() const;

  private:
    [[nodiscard]] BTreePageId allocatePage();
    void freePage(BTreePageId pageId);
    [[nodiscard]] BTreeNode &node(BTreePageId pageId);
    [[nodiscard]] const BTreeNode &node(BTreePageId pageId) const;
    [[nodiscard]] BTreePageId childFor(const BTreeNode &internal, const Value &key) const;
    [[nodiscard]] std::size_t childIndex(const BTreeNode &internal, const Value &key) const;
    [[nodiscard]] std::size_t minKeys() const noexcept;
    [[nodiscard]] BTreePageId leftmostLeaf() const;
    [[nodiscard]] std::vector<BTreePageId> pathToLeaf(const Value &key) const;

    void insertIntoLeaf(std::vector<BTreePageId> &path, const Value &key, RowId rowId);
    void splitLeaf(std::vector<BTreePageId> &path);
    void splitInternal(std::vector<BTreePageId> &path);
    void insertSeparator(std::vector<BTreePageId> &path, const Value &separator,
                         BTreePageId rightChild);

    void removeFromLeaf(std::vector<BTreePageId> &path, const Value &key, RowId rowId);
    void rebalanceAfterDelete(std::vector<BTreePageId> &path);
    void mergeLeaves(BTreePageId parentId, std::size_t leftChildIndex);
    void mergeInternals(BTreePageId parentId, std::size_t leftChildIndex);
    void borrowFromLeftLeaf(BTreePageId parentId, std::size_t childIndex);
    void borrowFromRightLeaf(BTreePageId parentId, std::size_t childIndex);
    void borrowFromLeftInternal(BTreePageId parentId, std::size_t childIndex);
    void borrowFromRightInternal(BTreePageId parentId, std::size_t childIndex);
    void collapseRootIfNeeded();
    void updateSeparator(BTreePageId parentId, std::size_t childIndex, const Value &separator);

    // Table::mutex_ serializes access; keep BTreeIndex movable for map storage.
    std::size_t maxKeysPerNode_;
    std::unordered_map<BTreePageId, BTreeNode> nodes_;
    std::vector<BTreePageId> freePageIds_;
    BTreePageId rootPageId_{1};
    BTreePageId nextPageId_{2};
    std::size_t keyCount_{0};
};

} // namespace VertexDB
