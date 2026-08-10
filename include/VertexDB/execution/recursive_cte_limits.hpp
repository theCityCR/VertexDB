#pragma once

// Production defaults for WITH RECURSIVE safety caps (docs/sql.md).
// Tests may temporarily lower these via recursiveCteLimits() with RAII restore.

#include <cstddef>

namespace VertexDB {

struct RecursiveCteLimits {
    std::size_t maxIterations{1000};
    std::size_t maxRows{100000};
};

[[nodiscard]] RecursiveCteLimits &recursiveCteLimits() noexcept;

} // namespace VertexDB
