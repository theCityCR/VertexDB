#pragma once

// Planner-facing read-only stats and index catalog interfaces (implemented by Table).

#include "VertexDB/common/index_expression.hpp"
#include "VertexDB/storage/histogram.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace VertexDB {

// Read-only planner input describing relation cardinality and analyzed column statistics.
class RelationStats {
  public:
    virtual ~RelationStats() = default;

    [[nodiscard]] virtual std::size_t rowCount() const = 0;
    [[nodiscard]] virtual std::optional<ColumnHistogram>
    columnHistogram(std::string_view column) const = 0;
};

// Read-only planner input describing available indexes and their observed key cardinality.
class IndexCatalogView {
  public:
    virtual ~IndexCatalogView() = default;

    [[nodiscard]] virtual bool hasIndex(std::string_view column) const = 0;
    // Exact ordered multi-column index match (size must equal the index key width).
    [[nodiscard]] virtual bool hasIndex(std::span<const std::string> columns) const {
        return columns.size() == 1 && hasIndex(columns.front());
    }
    [[nodiscard]] virtual bool hasExpressionIndex(const IndexExpression &expression) const = 0;
    [[nodiscard]] virtual std::optional<std::size_t>
    indexDistinctCount(std::string_view column) const = 0;
    [[nodiscard]] virtual std::optional<std::size_t>
    indexDistinctCount(std::span<const std::string> columns) const {
        return columns.size() == 1 ? indexDistinctCount(columns.front()) : std::nullopt;
    }
    [[nodiscard]] virtual std::optional<std::size_t>
    indexDistinctCount(const IndexExpression &expression) const = 0;
    // Ordered column lists for composite (width >= 2) column indexes; empty by default.
    [[nodiscard]] virtual std::vector<std::vector<std::string>>
    compositeIndexColumnLists() const {
        return {};
    }
};

} // namespace VertexDB
