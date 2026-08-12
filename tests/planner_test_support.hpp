#pragma once

// Shared helpers for planner_behavior_* GoogleTest TUs (stubs + temp executor).

#include "test_support.hpp"

#include "VertexDB/execution/query_executor.hpp"
#include "VertexDB/planner/query_planner.hpp"
#include "VertexDB/storage/relation_stats.hpp"

#include <optional>
#include <string_view>

namespace VertexDB {
namespace planner_test {

inline QueryExecutor makeExecutor(std::string_view suffix) {
    return makeTempExecutor("vertexdb-planner-", suffix);
}

class StubRelationStats final : public RelationStats {
  public:
    [[nodiscard]] std::size_t rowCount() const override { return 100; }

    [[nodiscard]] std::optional<ColumnHistogram>
    columnHistogram(std::string_view) const override {
        return std::nullopt;
    }
};

class StubIndexCatalog final : public IndexCatalogView {
  public:
    [[nodiscard]] bool hasIndex(std::string_view column) const override {
        return column == "id";
    }

    [[nodiscard]] bool hasExpressionIndex(const IndexExpression &) const override {
        return false;
    }

    [[nodiscard]] std::optional<std::size_t>
    indexDistinctCount(std::string_view column) const override {
        return column == "id" ? std::optional<std::size_t>{100} : std::nullopt;
    }

    [[nodiscard]] std::optional<std::size_t>
    indexDistinctCount(const IndexExpression &) const override {
        return std::nullopt;
    }
};

} // namespace planner_test
} // namespace VertexDB
