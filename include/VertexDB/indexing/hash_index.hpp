#pragma once

#include "VertexDB/storage/row.hpp"

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

    // Table::mutex_ serializes access; keep HashIndex movable for map storage.
    std::unordered_map<Value, std::vector<RowId>, ValueHash> entries_;
};

} // namespace VertexDB
