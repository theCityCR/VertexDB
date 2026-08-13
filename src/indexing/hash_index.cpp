#include "VertexDB/indexing/hash_index.hpp"

#include <algorithm>

namespace VertexDB {

std::size_t HashIndex::ValueHash::operator()(const Value &value) const {
    if (value.isNull()) {
        return 0x9e3779b97f4a7c15ULL;
    }
    if (value.isComposite()) {
        std::size_t seed = 0x9e3779b97f4a7c15ULL;
        ValueHash hasher;
        for (const auto &part : value.compositeParts()) {
            seed ^= hasher(part) + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
        }
        return seed;
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

void HashIndex::markDirty(const Value &key) { dirtyKeys_[key] = true; }

void HashIndex::insert(const Value &key, RowId rowId) {
    entries_[key].push_back(rowId);
    markDirty(key);
}

void HashIndex::remove(const Value &key, RowId rowId) {
    auto it = entries_.find(key);
    if (it == entries_.end()) {
        return;
    }
    auto &rowIds = it->second;
    std::erase(rowIds, rowId);
    markDirty(key);
    if (rowIds.empty()) {
        entries_.erase(it);
    }
}

void HashIndex::clear() {
    entries_.clear();
    dirtyKeys_.clear();
    fullReplaceDirty_ = true;
}

std::vector<RowId> HashIndex::find(const Value &key) const {
    auto it = entries_.find(key);
    if (it == entries_.end()) {
        return {};
    }
    return it->second;
}

std::size_t HashIndex::size() const { return entries_.size(); }

HashIndexSnapshot HashIndex::exportBuckets() const {
    HashIndexSnapshot snapshot;
    snapshot.buckets.reserve(entries_.size());
    for (const auto &[key, rowIds] : entries_) {
        snapshot.buckets.emplace_back(key, rowIds);
    }
    return snapshot;
}

void HashIndex::replaceFromBuckets(HashIndexSnapshot snapshot) {
    entries_.clear();
    for (auto &[key, rowIds] : snapshot.buckets) {
        entries_.emplace(std::move(key), std::move(rowIds));
    }
    clearDirtyPages();
}

void HashIndex::clearDirtyPages() noexcept {
    dirtyKeys_.clear();
    fullReplaceDirty_ = false;
}

bool HashIndex::hasDirtyPages() const noexcept {
    return fullReplaceDirty_ || !dirtyKeys_.empty();
}

HashIndexSnapshot HashIndex::takeDirtyBuckets() {
    HashIndexSnapshot snapshot;
    snapshot.replaceAll = fullReplaceDirty_;
    if (fullReplaceDirty_) {
        snapshot.buckets = exportBuckets().buckets;
    } else {
        snapshot.buckets.reserve(dirtyKeys_.size());
        for (const auto &[key, _] : dirtyKeys_) {
            auto it = entries_.find(key);
            if (it == entries_.end()) {
                snapshot.buckets.emplace_back(key, std::vector<RowId>{});
            } else {
                snapshot.buckets.emplace_back(it->first, it->second);
            }
        }
    }
    clearDirtyPages();
    return snapshot;
}

void HashIndex::applyDirtyBuckets(const HashIndexSnapshot &dirty) {
    if (dirty.replaceAll) {
        HashIndexSnapshot copy = dirty;
        copy.replaceAll = false;
        replaceFromBuckets(std::move(copy));
        return;
    }
    for (const auto &[key, rowIds] : dirty.buckets) {
        if (rowIds.empty()) {
            entries_.erase(key);
        } else {
            entries_[key] = rowIds;
        }
    }
    clearDirtyPages();
}

} // namespace VertexDB
