#pragma once

#include "VertexDB/storage/row.hpp"

#include <unordered_map>
#include <utility>
#include <vector>

namespace VertexDB {

struct HashIndexSnapshot {
    bool replaceAll{false};
    std::vector<std::pair<Value, std::vector<RowId>>> buckets;
};

class HashIndex {
  public:
    void insert(const Value &key, RowId rowId);
    void remove(const Value &key, RowId rowId);
    void clear();
    [[nodiscard]] std::vector<RowId> find(const Value &key) const;
    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] HashIndexSnapshot exportBuckets() const;
    void replaceFromBuckets(HashIndexSnapshot snapshot);
    void clearDirtyPages() noexcept;
    [[nodiscard]] bool hasDirtyPages() const noexcept;
    // Full replace when clear()/rebuild dirtied the map; otherwise only touched keys.
    [[nodiscard]] HashIndexSnapshot takeDirtyBuckets();
    void applyDirtyBuckets(const HashIndexSnapshot &dirty);

  private:
    struct ValueHash {
        [[nodiscard]] std::size_t operator()(const Value &value) const;
    };

    void markDirty(const Value &key);

    // Table::mutex_ serializes access; keep HashIndex movable for map storage.
    std::unordered_map<Value, std::vector<RowId>, ValueHash> entries_;
    std::unordered_map<Value, bool, ValueHash> dirtyKeys_;
    bool fullReplaceDirty_{false};
};

} // namespace VertexDB
