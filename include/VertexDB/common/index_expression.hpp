#pragma once

#include "VertexDB/parser/ast.hpp"
#include "VertexDB/storage/row.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace VertexDB {

[[nodiscard]] std::string indexExpressionToString(const IndexExpression &expression);
[[nodiscard]] std::optional<IndexExpression> parseIndexExpressionString(std::string_view text);
[[nodiscard]] Value evaluateIndexExpression(
    const IndexExpression &expression, const Row &row,
    const std::function<std::optional<std::size_t>(std::string_view)> &lookup);

// Persistence encoding: column indexes store the column name; expression indexes use this prefix.
inline constexpr std::string_view kExpressionIndexPrefix = "expr:";

[[nodiscard]] std::string encodeIndexDefinitionColumn(const std::string &column,
                                                      const std::optional<IndexExpression> &expression);
[[nodiscard]] std::pair<std::string, std::optional<IndexExpression>>
decodeIndexDefinitionColumn(std::string_view encoded);

} // namespace VertexDB
