#pragma once

// Case-insensitive string compare helpers (header-only).

#include <cctype>
#include <string_view>

namespace VertexDB {

[[nodiscard]] inline bool equalsIgnoreCase(std::string_view lhs, std::string_view rhs) {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        const auto left = static_cast<unsigned char>(lhs[i]);
        const auto right = static_cast<unsigned char>(rhs[i]);
        if (std::toupper(left) != std::toupper(right)) {
            return false;
        }
    }
    return true;
}

} // namespace VertexDB
