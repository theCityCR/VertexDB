#pragma once

// Table-level UNIQUE / PRIMARY KEY (including multi-column composite keys).

#include <string>
#include <vector>

namespace VertexDB {

struct UniqueConstraint {
    std::vector<std::string> columns;
    bool primaryKey{false};
};

} // namespace VertexDB
