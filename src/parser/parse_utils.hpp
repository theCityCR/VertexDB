#pragma once

// Internal parser helpers (literals, identifiers). Used by Parser TUs under src/parser/.
// Not part of the public include surface.

#include "VertexDB/common/string_utils.hpp"

#include <charconv>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace VertexDB {
namespace parser_detail {

inline std::int64_t parseIntLiteral(std::string_view text) {
    std::int64_t result{};
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), result);
    if (ec != std::errc{} || ptr != text.data() + text.size()) {
        throw std::runtime_error("invalid integer literal");
    }
    return result;
}

[[nodiscard]] inline bool isSelectClauseKeyword(std::string_view lexeme) {
    return equalsIgnoreCase(lexeme, "WHERE") || equalsIgnoreCase(lexeme, "JOIN") ||
           equalsIgnoreCase(lexeme, "GROUP") || equalsIgnoreCase(lexeme, "ORDER") ||
           equalsIgnoreCase(lexeme, "LIMIT") || equalsIgnoreCase(lexeme, "UNION") ||
           equalsIgnoreCase(lexeme, "INTERSECT") || equalsIgnoreCase(lexeme, "EXCEPT") ||
           equalsIgnoreCase(lexeme, "HAVING") || equalsIgnoreCase(lexeme, "ON") ||
           equalsIgnoreCase(lexeme, "LEFT") || equalsIgnoreCase(lexeme, "RIGHT") ||
           equalsIgnoreCase(lexeme, "FULL") || equalsIgnoreCase(lexeme, "INNER") ||
           equalsIgnoreCase(lexeme, "OUTER") || equalsIgnoreCase(lexeme, "CROSS") ||
           equalsIgnoreCase(lexeme, "ALL");
}

[[nodiscard]] inline bool isJoinIntroducer(std::string_view lexeme) {
    return equalsIgnoreCase(lexeme, "JOIN") || equalsIgnoreCase(lexeme, "LEFT") ||
           equalsIgnoreCase(lexeme, "RIGHT") || equalsIgnoreCase(lexeme, "FULL") ||
           equalsIgnoreCase(lexeme, "INNER") || equalsIgnoreCase(lexeme, "CROSS");
}

[[nodiscard]] inline std::optional<std::string_view> columnQualifier(std::string_view name) {
    const auto dot = name.find('.');
    if (dot == std::string_view::npos) {
        return std::nullopt;
    }
    return name.substr(0, dot);
}

[[nodiscard]] inline bool refersToOuterTable(std::string_view column, std::string_view innerTable,
                                             const std::vector<std::string> &outerTables) {
    if (const auto table = columnQualifier(column)) {
        if (equalsIgnoreCase(*table, innerTable)) {
            return false;
        }
        for (const auto &outer : outerTables) {
            if (equalsIgnoreCase(*table, outer)) {
                return true;
            }
        }
        // Qualified name that is not the inner table is treated as an outer reference.
        return !outerTables.empty();
    }
    return false;
}

} // namespace parser_detail
} // namespace VertexDB
