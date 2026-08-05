#pragma once

#include <mutex>
#include <shared_mutex>

namespace VertexDB {

class LockManager {
  public:
    [[nodiscard]] std::shared_lock<std::shared_mutex> acquireRead() const;
    [[nodiscard]] std::unique_lock<std::shared_mutex> acquireWrite() const;

  private:
    mutable std::shared_mutex mutex_;
};

} // namespace VertexDB
