#include "VertexDB/storage/buffer_pool.hpp"

#include <stdexcept>

namespace VertexDB {

BufferPool::BufferPool(std::size_t capacity) : capacity_(capacity) {
    if (capacity_ == 0) {
        throw std::invalid_argument("buffer pool capacity must be positive");
    }
}

void BufferPool::put(Page page) {
    if (auto it = pages_.find(page.id); it != pages_.end()) {
        it->second.first = std::move(page);
        touch(it->second.second);
        return;
    }

    lru_.push_front(page.id);
    pages_.emplace(page.id, std::make_pair(std::move(page), lru_.begin()));
    evictIfNeeded();
}

std::optional<Page> BufferPool::get(PageId pageId) {
    auto it = pages_.find(pageId);
    if (it == pages_.end()) {
        return std::nullopt;
    }
    touch(it->second.second);
    return it->second.first;
}

bool BufferPool::contains(PageId pageId) const { return pages_.contains(pageId); }

std::size_t BufferPool::size() const noexcept { return pages_.size(); }

std::size_t BufferPool::capacity() const noexcept { return capacity_; }

void BufferPool::touch(std::list<PageId>::iterator it) { lru_.splice(lru_.begin(), lru_, it); }

void BufferPool::evictIfNeeded() {
    while (pages_.size() > capacity_) {
        const auto evicted = lru_.back();
        lru_.pop_back();
        pages_.erase(evicted);
    }
}

} // namespace VertexDB
