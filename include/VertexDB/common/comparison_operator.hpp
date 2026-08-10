#pragma once

// Comparison operators shared by predicates, joins, and ordered indexes.

#include <cstdint>

namespace VertexDB {

enum class ComparisonOperator : std::uint8_t {
    Equal,
    Greater,
    Less,
};

} // namespace VertexDB
