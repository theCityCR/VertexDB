#pragma once

#include "VertexDB/common/string_utils.hpp"

#include <charconv>
#include <cstdint>
#include <stdexcept>
#include <string_view>

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
           equalsIgnoreCase(lexeme, "HAVING") || equalsIgnoreCase(lexeme, "ON") ||
           equalsIgnoreCase(lexeme, "LEFT") || equalsIgnoreCase(lexeme, "RIGHT") ||
           equalsIgnoreCase(lexeme, "FULL") || equalsIgnoreCase(lexeme, "INNER") ||
           equalsIgnoreCase(lexeme, "OUTER") || equalsIgnoreCase(lexeme, "CROSS");
}

[[nodiscard]] inline bool isJoinIntroducer(std::string_view lexeme) {
    return equalsIgnoreCase(lexeme, "JOIN") || equalsIgnoreCase(lexeme, "LEFT") ||
           equalsIgnoreCase(lexeme, "RIGHT") || equalsIgnoreCase(lexeme, "FULL") ||
           equalsIgnoreCase(lexeme, "INNER") || equalsIgnoreCase(lexeme, "CROSS");
}

} // namespace parser_detail
} // namespace VertexDB