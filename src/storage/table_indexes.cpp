#include "VertexDB/storage/table.hpp"

#include "VertexDB/common/index_expression.hpp"

#include <algorithm>
#include <mutex>
#include <shared_mutex>

namespace VertexDB {

std::optional<std::size_t> Table::indexDistinctCount(std::string_view column) const {
    std::shared_lock lock{mutex_};
    for (const auto &[indexName, columnIndex] : indexColumns_) {
        if (indexExpressions_.contains(indexName)) {
            continue;
        }
        if (schema_[columnIndex].name == column) {
            return indexes_.at(indexName).size();
        }
    }
    return std::nullopt;
}

std::optional<std::size_t> Table::indexDistinctCount(const IndexExpression &expression) const {
    std::shared_lock lock{mutex_};
    for (const auto &[indexName, stored] : indexExpressions_) {
        if (stored == expression) {
            return indexes_.at(indexName).size();
        }
    }
    return std::nullopt;
}

std::optional<std::vector<RowId>> Table::indexedLookup(std::string_view column,
                                                       const Value &value) const {
    std::shared_lock lock{mutex_};
    for (const auto &[indexName, columnIndex] : indexColumns_) {
        if (indexExpressions_.contains(indexName)) {
            continue;
        }
        if (schema_[columnIndex].name == column) {
            return indexes_.at(indexName).find(value);
        }
    }
    return std::nullopt;
}

std::optional<std::vector<RowId>> Table::indexedLookup(const IndexExpression &expression,
                                                       const Value &value) const {
    std::shared_lock lock{mutex_};
    for (const auto &[indexName, stored] : indexExpressions_) {
        if (stored == expression) {
            return indexes_.at(indexName).find(value);
        }
    }
    return std::nullopt;
}

std::optional<std::vector<RowId>>
Table::orderedLookup(std::string_view column, ComparisonOperator op, const Value &value) const {
    std::shared_lock lock{mutex_};
    for (const auto &[indexName, columnIndex] : indexColumns_) {
        if (indexExpressions_.contains(indexName)) {
            continue;
        }
        if (schema_[columnIndex].name != column) {
            continue;
        }
        if (op == ComparisonOperator::Equal) {
            return orderedIndexes_.at(indexName).find(value);
        }
        if (op == ComparisonOperator::Greater) {
            return orderedIndexes_.at(indexName).greaterThan(value);
        }
        if (op == ComparisonOperator::Less) {
            return orderedIndexes_.at(indexName).lessThan(value);
        }
    }
    return std::nullopt;
}

std::optional<std::vector<RowId>>
Table::orderedLookup(const IndexExpression &expression, ComparisonOperator op,
                     const Value &value) const {
    std::shared_lock lock{mutex_};
    for (const auto &[indexName, stored] : indexExpressions_) {
        if (stored != expression) {
            continue;
        }
        if (op == ComparisonOperator::Equal) {
            return orderedIndexes_.at(indexName).find(value);
        }
        if (op == ComparisonOperator::Greater) {
            return orderedIndexes_.at(indexName).greaterThan(value);
        }
        if (op == ComparisonOperator::Less) {
            return orderedIndexes_.at(indexName).lessThan(value);
        }
    }
    return std::nullopt;
}

bool Table::hasIndex(std::string_view column) const {
    std::shared_lock lock{mutex_};
    return std::ranges::any_of(indexColumns_, [&](const auto &item) {
        return !indexExpressions_.contains(item.first) && schema_[item.second].name == column;
    });
}

bool Table::hasExpressionIndex(const IndexExpression &expression) const {
    std::shared_lock lock{mutex_};
    return std::ranges::any_of(indexExpressions_,
                               [&](const auto &item) { return item.second == expression; });
}

std::vector<std::string> Table::listIndexes() const {
    std::shared_lock lock{mutex_};
    std::vector<std::string> names;
    names.reserve(indexes_.size());
    for (const auto &[name, _] : indexes_) {
        names.push_back(name);
    }
    return names;
}

std::vector<IndexDefinition> Table::indexDefinitions() const {
    std::shared_lock lock{mutex_};
    std::vector<IndexDefinition> definitions;
    definitions.reserve(indexColumns_.size());
    for (const auto &[name, columnIndex] : indexColumns_) {
        IndexDefinition definition;
        definition.name = name;
        definition.column = schema_.at(columnIndex).name;
        if (auto it = indexExpressions_.find(name); it != indexExpressions_.end()) {
            definition.expression = it->second;
        }
        definitions.push_back(std::move(definition));
    }
    return definitions;
}

std::optional<std::vector<BTreeNode>>
Table::orderedIndexNodesSnapshot(std::string_view indexName) const {
    std::shared_lock lock{mutex_};
    auto it = orderedIndexes_.find(std::string{indexName});
    if (it == orderedIndexes_.end()) {
        return std::nullopt;
    }
    return it->second.nodesSnapshot();
}

bool Table::registerIndex(std::string name, std::size_t columnIndex,
                          std::optional<IndexExpression> expression, bool rebuild) {
    if (indexes_.contains(name) || indexColumns_.contains(name)) {
        return false;
    }
    indexColumns_.emplace(name, columnIndex);
    if (expression) {
        indexExpressions_.emplace(name, std::move(*expression));
    }
    indexes_.try_emplace(name);
    orderedIndexes_.try_emplace(name);
    if (rebuild) {
        rebuildIndexes();
    }
    return true;
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

Value Table::indexKeyForRow(const std::string &indexName, const Row &row) const {
    if (auto it = indexExpressions_.find(indexName); it != indexExpressions_.end()) {
        return evaluateIndexExpression(it->second, row, [this](std::string_view column) {
            return columnIndex(column);
        });
    }
    return row[indexColumns_.at(indexName)];
}

void Table::addRowToIndexes(RowId rowId) {
    const auto *row = rowStore_->get(rowId);
    if (row == nullptr) {
        return;
    }
    for (auto &[name, index] : indexes_) {
        index.insert(indexKeyForRow(name, *row), rowId);
    }
    for (auto &[name, index] : orderedIndexes_) {
        index.insert(indexKeyForRow(name, *row), rowId);
    }
}

void Table::rebuildIndexes() {
    for (auto &[_, index] : indexes_) {
        index.clear();
    }
    for (auto &[_, index] : orderedIndexes_) {
        index.clear();
    }
    for (RowId rowId = 0; rowId < rowStore_->capacity(); ++rowId) {
        addRowToIndexes(rowId);
    }
}

} // namespace VertexDB
