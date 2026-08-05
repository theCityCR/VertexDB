#pragma once

#include "VertexDB/storage/row.hpp"

#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace VertexDB {

class HashIndex {
  public:
    void insert(const Value &key, RowId rowId);
    void remove(const Value &key, RowId rowId);
    void clear();
    [[nodiscard]] std::vector<RowId> find(const Value &key) const;
    [[nodiscard]] std::size_t size() const;

  private:
    struct ValueHash {
        [[nodiscard]] std::size_t operator()(const Value &value) const;
    };

    std::unordered_map<Value, std::vector<RowId>, ValueHash> entries_;
    mutable std::shared_mutex mutex_;
};

} // namespace VertexDB
