#pragma once

#include "VertexDB/common/value.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace VertexDB {

// SQL LIKE with '%' (any sequence) and '_' (single char). No escape clause.
[[nodiscard]] bool matchLikePattern(std::string_view text, std::string_view pattern);

// ECMAScript regex via std::regex; invalid patterns throw std::runtime_error.
[[nodiscard]] bool matchRegexPattern(std::string_view text, std::string_view pattern);

// Prefix form: literal% with no other wildcards → optional literal prefix.
[[nodiscard]] std::optional<std::string> likePrefixLiteral(std::string_view pattern);

// Contains form: %literal% with no other wildcards → optional literal needle.
[[nodiscard]] std::optional<std::string> likeContainsLiteral(std::string_view pattern);

[[nodiscard]] std::vector<std::string> extractTrigrams(std::string_view text);

} // namespace VertexDB
