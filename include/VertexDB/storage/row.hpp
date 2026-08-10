#pragma once

// Row = vector of Value; RowId is the stable heap identity.

#include "VertexDB/common/value.hpp"

#include <vector>

namespace VertexDB {

using Row = std::vector<Value>;
using RowId = std::size_t;

} // namespace VertexDB
