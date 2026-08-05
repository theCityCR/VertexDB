#include "VertexDB/indexing/hash_index.hpp"

#include <algorithm>
#include <mutex>
#include <shared_mutex>

namespace VertexDB {

std::size_t HashIndex::ValueHash::operator()(const Value &value) const {
    if (value.isNull()) {
        return 0x9e3779b97f4a7c15ULL;
    }
    switch (value.type()) {
    case ColumnType::Int:
        return std::hash<std::int64_t>{}(std::get<std::int64_t>(value.data()));
    case ColumnType::Double:
        return std::hash<double>{}(std::get<double>(value.data()));
    case ColumnType::String:
        return std::hash<std::string>{}(std::get<std::string>(value.data()));
    }
    return 0;
}

void HashIndex::insert(const Value &key, RowId rowId) {
    std::unique_lock lock{mutex_};
    entries_[key].push_back(rowId);
}

void HashIndex::remove(const Value &key, RowId rowId) {
    std::unique_lock lock{mutex_};
    auto it = entries_.find(key);
    if (it == entries_.end()) {
        return;
    }
    auto &rowIds = it->second;
    std::erase(rowIds, rowId);
    if (rowIds.empty()) {
        entries_.erase(it);
    }
}

void HashIndex::clear() {
    std::unique_lock lock{mutex_};
    entries_.clear();
}

std::vector<RowId> HashIndex::find(const Value &key) const {
    std::shared_lock lock{mutex_};
    auto it = entries_.find(key);
    if (it == entries_.end()) {
        return {};
    }
    return it->second;
}

std::size_t HashIndex::size() const {
    std::shared_lock lock{mutex_};
    return entries_.size();
}

} // namespace VertexDB
