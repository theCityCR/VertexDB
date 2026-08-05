#pragma once

#include "VertexDB/storage/row.hpp"

#include <map>
#include <optional>
#include <shared_mutex>
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
    explicit BTreeIndex(std::size_t maxKeysPerLeaf = 64);

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
    void rebuildLayout();

    std::map<Value, std::vector<RowId>> entries_;
    std::size_t maxKeysPerLeaf_;
    std::vector<BTreeNode> nodes_;
    mutable std::shared_mutex mutex_;
};

} // namespace VertexDB
