#pragma once

#include <cstdint>

namespace VertexDB {

enum class ComparisonOperator : std::uint8_t {
    Equal,
    Greater,
    Less,
};

} // namespace VertexDB
