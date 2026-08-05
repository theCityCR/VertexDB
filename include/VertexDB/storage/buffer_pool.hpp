#pragma once

#include <cstddef>
#include <cstdint>
#include <list>
#include <optional>
#include <unordered_map>
#include <vector>

namespace VertexDB {

using PageId = std::uint64_t;

struct Page {
    PageId id{};
    std::vector<std::byte> bytes;
    bool dirty{false};
};

class BufferPool {
  public:
    explicit BufferPool(std::size_t capacity);

    void put(Page page);
    [[nodiscard]] std::optional<Page> get(PageId pageId);
    [[nodiscard]] bool contains(PageId pageId) const;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::size_t capacity() const noexcept;

  private:
    void touch(std::list<PageId>::iterator it);
    void evictIfNeeded();

    std::size_t capacity_;
    std::list<PageId> lru_;
    std::unordered_map<PageId, std::pair<Page, std::list<PageId>::iterator>> pages_;
};

} // namespace VertexDB
