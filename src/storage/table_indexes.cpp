#include "VertexDB/storage/table.hpp"

#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <vector>

namespace VertexDB {

std::optional<std::size_t> Table::indexDistinctCount(std::string_view column) const {
    std::shared_lock lock{mutex_};
    return indexManager_.indexDistinctCount(column, schema_);
}

std::optional<std::size_t> Table::indexDistinctCount(const IndexExpression &expression) const {
    std::shared_lock lock{mutex_};
    return indexManager_.indexDistinctCount(expression);
}

std::optional<std::vector<RowId>> Table::indexedLookup(std::string_view column,
                                                       const Value &value) const {
    std::shared_lock lock{mutex_};
    return indexManager_.indexedLookup(column, value, schema_);
}

std::optional<std::vector<RowId>> Table::indexedLookup(std::span<const std::string> columns,
                                                       const Value &key) const {
    std::shared_lock lock{mutex_};
    return indexManager_.indexedLookup(columns, key, schema_);
}

std::optional<std::vector<RowId>> Table::indexedLookup(const IndexExpression &expression,
                                                       const Value &value) const {
    std::shared_lock lock{mutex_};
    return indexManager_.indexedLookup(expression, value);
}

std::optional<std::vector<RowId>>
Table::orderedLookup(std::string_view column, ComparisonOperator op, const Value &value) const {
    std::shared_lock lock{mutex_};
    return indexManager_.orderedLookup(column, op, value, schema_);
}

std::optional<std::vector<RowId>>
Table::orderedLookup(const IndexExpression &expression, ComparisonOperator op,
                     const Value &value) const {
    std::shared_lock lock{mutex_};
    return indexManager_.orderedLookup(expression, op, value);
}

bool Table::hasIndex(std::string_view column) const {
    std::shared_lock lock{mutex_};
    return indexManager_.hasIndex(column, schema_);
}

bool Table::hasIndex(std::span<const std::string> columns) const {
    std::shared_lock lock{mutex_};
    return indexManager_.hasIndex(columns, schema_);
}

bool Table::hasExpressionIndex(const IndexExpression &expression) const {
    std::shared_lock lock{mutex_};
    return indexManager_.hasExpressionIndex(expression);
}

std::vector<std::string> Table::listIndexes() const {
    std::shared_lock lock{mutex_};
    return indexManager_.listIndexes();
}

std::vector<IndexDefinition> Table::indexDefinitions() const {
    std::shared_lock lock{mutex_};
    return indexManager_.indexDefinitions(schema_);
}

std::optional<std::vector<BTreeNode>>
Table::orderedIndexNodesSnapshot(std::string_view indexName) const {
    std::shared_lock lock{mutex_};
    return indexManager_.orderedIndexNodesSnapshot(indexName);
}

bool Table::registerIndex(std::string name, std::vector<std::size_t> columnIndexes,
                          std::optional<IndexExpression> expression, bool rebuild) {
    return indexManager_.registerIndex(std::move(name), std::move(columnIndexes),
                                       std::move(expression), rebuild, *rowStore_, schema_);
}

bool Table::registerIndex(std::string name, std::size_t columnIndex,
                          std::optional<IndexExpression> expression, bool rebuild) {
    return registerIndex(std::move(name), std::vector<std::size_t>{columnIndex},
                         std::move(expression), rebuild);
}

bool Table::createIndex(std::string name, std::string column) {
    return createIndex(std::move(name), std::vector<std::string>{std::move(column)});
}

bool Table::createIndex(std::string name, std::vector<std::string> columns) {
    if (columns.empty()) {
        return false;
    }
    std::vector<std::size_t> indexes;
    indexes.reserve(columns.size());
    for (const auto &column : columns) {
        auto indexColumn = columnIndex(column);
        if (!indexColumn) {
            return false;
        }
        indexes.push_back(*indexColumn);
    }
    std::unique_lock lock{mutex_};
    return registerIndex(std::move(name), std::move(indexes), std::nullopt, true);
}

bool Table::createIndex(std::string name, IndexExpression expression) {
    auto indexColumn = columnIndex(expression.column);
    if (!indexColumn) {
        return false;
    }
    std::unique_lock lock{mutex_};
    return registerIndex(std::move(name), *indexColumn, std::move(expression), true);
}

bool Table::createIndexWithoutRebuild(std::string name, std::string column) {
    return createIndexWithoutRebuild(std::move(name), std::vector<std::string>{std::move(column)});
}

bool Table::createIndexWithoutRebuild(std::string name, std::vector<std::string> columns) {
    if (columns.empty()) {
        return false;
    }
    std::vector<std::size_t> indexes;
    indexes.reserve(columns.size());
    for (const auto &column : columns) {
        auto indexColumn = columnIndex(column);
        if (!indexColumn) {
            return false;
        }
        indexes.push_back(*indexColumn);
    }
    std::unique_lock lock{mutex_};
    return registerIndex(std::move(name), std::move(indexes), std::nullopt, false);
}

bool Table::createIndexWithoutRebuild(std::string name, IndexExpression expression) {
    auto indexColumn = columnIndex(expression.column);
    if (!indexColumn) {
        return false;
    }
    std::unique_lock lock{mutex_};
    return registerIndex(std::move(name), *indexColumn, std::move(expression), false);
}

bool Table::dropIndex(std::string_view name) {
    std::unique_lock lock{mutex_};
    return indexManager_.dropIndex(name);
}

void Table::addRowToIndexes(RowId rowId) {
    indexManager_.addRowToIndexes(rowId, *rowStore_, schema_);
}

void Table::rebuildIndexes() { indexManager_.rebuildIndexes(*rowStore_, schema_); }

} // namespace VertexDB
