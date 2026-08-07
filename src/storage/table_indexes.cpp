#include "VertexDB/storage/table.hpp"

#include <mutex>
#include <shared_mutex>

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

bool Table::registerIndex(std::string name, std::size_t columnIndex,
                          std::optional<IndexExpression> expression, bool rebuild) {
    return indexManager_.registerIndex(std::move(name), columnIndex, std::move(expression), rebuild,
                                       *rowStore_, schema_);
}

bool Table::createIndex(std::string name, std::string column) {
    auto indexColumn = columnIndex(column);
    if (!indexColumn) {
        return false;
    }
    std::unique_lock lock{mutex_};
    return registerIndex(std::move(name), *indexColumn, std::nullopt, true);
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
    auto indexColumn = columnIndex(column);
    if (!indexColumn) {
        return false;
    }
    std::unique_lock lock{mutex_};
    return registerIndex(std::move(name), *indexColumn, std::nullopt, false);
}

bool Table::createIndexWithoutRebuild(std::string name, IndexExpression expression) {
    auto indexColumn = columnIndex(expression.column);
    if (!indexColumn) {
        return false;
    }
    std::unique_lock lock{mutex_};
    return registerIndex(std::move(name), *indexColumn, std::move(expression), false);
}

void Table::addRowToIndexes(RowId rowId) {
    indexManager_.addRowToIndexes(rowId, *rowStore_, schema_);
}

void Table::rebuildIndexes() { indexManager_.rebuildIndexes(*rowStore_, schema_); }

} // namespace VertexDB
