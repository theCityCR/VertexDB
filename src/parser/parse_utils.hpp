#pragma once

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

} // namespace parser_detail
} // namespace VertexDB
