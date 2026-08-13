#pragma once

// Index expression shapes and helpers shared by parser, planner, indexes, and persistence.
// Lives in common/ so storage does not depend on parser/ast.hpp for index metadata.

#include "VertexDB/common/value.hpp"
#include "VertexDB/storage/row.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace VertexDB {

struct IndexExpression {
    enum class Kind { Column, Negate, Add, Subtract, Trigram };

    Kind kind{Kind::Column};
    std::string column;
    Value literal{};

    [[nodiscard]] friend bool operator==(const IndexExpression &lhs,
                                         const IndexExpression &rhs) = default;
};

[[nodiscard]] std::string indexExpressionToString(const IndexExpression &expression);
[[nodiscard]] std::optional<IndexExpression> parseIndexExpressionString(std::string_view text);
[[nodiscard]] Value evaluateIndexExpression(
    const IndexExpression &expression, const Row &row,
    const std::function<std::optional<std::size_t>(std::string_view)> &lookup);

// Persistence encoding: column indexes store the column name; expression indexes use this prefix;
// multi-column indexes use cols:a,b.
inline constexpr std::string_view kExpressionIndexPrefix = "expr:";
inline constexpr std::string_view kCompositeIndexPrefix = "cols:";

[[nodiscard]] std::string encodeIndexDefinitionColumn(const std::string &column,
                                                      const std::optional<IndexExpression> &expression);
[[nodiscard]] std::string encodeIndexDefinitionColumns(const std::vector<std::string> &columns,
                                                       const std::optional<IndexExpression> &expression);
[[nodiscard]] std::pair<std::string, std::optional<IndexExpression>>
decodeIndexDefinitionColumn(std::string_view encoded);
[[nodiscard]] std::pair<std::vector<std::string>, std::optional<IndexExpression>>
decodeIndexDefinitionColumns(std::string_view encoded);

} // namespace VertexDB
