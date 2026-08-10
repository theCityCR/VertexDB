#include "VertexDB/indexing/index_manager.hpp"

#include "VertexDB/common/string_pattern.hpp"

#include <algorithm>
#include <utility>
#include <variant>

namespace VertexDB {
namespace {

std::optional<std::vector<RowId>> orderedFind(const BTreeIndex &index, ComparisonOperator op,
                                              const Value &value) {
    if (op == ComparisonOperator::Equal) {
        return index.find(value);
    }
    if (op == ComparisonOperator::Greater) {
        return index.greaterThan(value);
    }
    if (op == ComparisonOperator::Less) {
        return index.lessThan(value);
    }
    return std::nullopt;
}

std::optional<std::size_t> findColumn(std::span<const Column> schema, std::string_view column) {
    auto it =
        std::ranges::find_if(schema, [column](const Column &item) { return item.name == column; });
    if (it == schema.end()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(std::distance(schema.begin(), it));
}

} // namespace

std::optional<std::size_t>
IndexManager::indexDistinctCount(std::string_view column,
                                 std::span<const Column> schema) const {
    for (const auto &[indexName, columnIndex] : indexColumns_) {
        if (!indexExpressions_.contains(indexName) && schema[columnIndex].name == column) {
            return indexes_.at(indexName).size();
        }
    }
    return std::nullopt;
}

std::optional<std::size_t>
IndexManager::indexDistinctCount(const IndexExpression &expression) const {
    for (const auto &[indexName, stored] : indexExpressions_) {
        if (stored == expression) {
            return indexes_.at(indexName).size();
        }
    }
    return std::nullopt;
}

std::optional<std::vector<RowId>>
IndexManager::indexedLookup(std::string_view column, const Value &value,
                            std::span<const Column> schema) const {
    for (const auto &[indexName, columnIndex] : indexColumns_) {
        if (!indexExpressions_.contains(indexName) && schema[columnIndex].name == column) {
            return indexes_.at(indexName).find(value);
        }
    }
    return std::nullopt;
}

std::optional<std::vector<RowId>>
IndexManager::indexedLookup(const IndexExpression &expression, const Value &value) const {
    for (const auto &[indexName, stored] : indexExpressions_) {
        if (stored == expression) {
            return indexes_.at(indexName).find(value);
        }
    }
    return std::nullopt;
}

std::optional<std::vector<RowId>>
IndexManager::orderedLookup(std::string_view column, ComparisonOperator op, const Value &value,
                            std::span<const Column> schema) const {
    for (const auto &[indexName, columnIndex] : indexColumns_) {
        if (!indexExpressions_.contains(indexName) && schema[columnIndex].name == column) {
            return orderedFind(orderedIndexes_.at(indexName), op, value);
        }
    }
    return std::nullopt;
}

std::optional<std::vector<RowId>>
IndexManager::orderedLookup(const IndexExpression &expression, ComparisonOperator op,
                            const Value &value) const {
    for (const auto &[indexName, stored] : indexExpressions_) {
        if (stored == expression) {
            return orderedFind(orderedIndexes_.at(indexName), op, value);
        }
    }
    return std::nullopt;
}

bool IndexManager::hasIndex(std::string_view column, std::span<const Column> schema) const {
    return std::ranges::any_of(indexColumns_, [&](const auto &item) {
        return !indexExpressions_.contains(item.first) && schema[item.second].name == column;
    });
}

bool IndexManager::hasExpressionIndex(const IndexExpression &expression) const {
    return std::ranges::any_of(indexExpressions_,
                               [&](const auto &item) { return item.second == expression; });
}

std::vector<std::string> IndexManager::listIndexes() const {
    std::vector<std::string> names;
    names.reserve(indexes_.size());
    for (const auto &[name, _] : indexes_) {
        names.push_back(name);
    }
    return names;
}

std::vector<IndexDefinition>
IndexManager::indexDefinitions(std::span<const Column> schema) const {
    std::vector<IndexDefinition> definitions;
    definitions.reserve(indexColumns_.size());
    for (const auto &[name, columnIndex] : indexColumns_) {
        IndexDefinition definition;
        definition.name = name;
        definition.column = schema[columnIndex].name;
        if (auto it = indexExpressions_.find(name); it != indexExpressions_.end()) {
            definition.expression = it->second;
        }
        definitions.push_back(std::move(definition));
    }
    return definitions;
}

std::optional<std::vector<BTreeNode>>
IndexManager::orderedIndexNodesSnapshot(std::string_view indexName) const {
    auto it = orderedIndexes_.find(std::string{indexName});
    if (it == orderedIndexes_.end()) {
        return std::nullopt;
    }
    return it->second.nodesSnapshot();
}

bool IndexManager::registerIndex(std::string name, std::size_t columnIndex,
                                 std::optional<IndexExpression> expression, bool rebuild,
                                 const RowStore &rowStore, std::span<const Column> schema) {
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
        rebuildIndexes(rowStore, schema);
    }
    return true;
}

bool IndexManager::dropIndex(std::string_view name) {
    const std::string key{name};
    if (!indexColumns_.contains(key) && !indexes_.contains(key) &&
        !orderedIndexes_.contains(key) && !indexExpressions_.contains(key)) {
        return false;
    }
    indexColumns_.erase(key);
    indexExpressions_.erase(key);
    indexes_.erase(key);
    orderedIndexes_.erase(key);
    return true;
}

Value IndexManager::indexKeyForRow(const std::string &indexName, const Row &row,
                                   std::span<const Column> schema) const {
    if (auto it = indexExpressions_.find(indexName); it != indexExpressions_.end()) {
        return evaluateIndexExpression(it->second, row,
                                       [schema](std::string_view column) {
                                           return findColumn(schema, column);
                                       });
    }
    return row[indexColumns_.at(indexName)];
}

void IndexManager::addRowToIndexes(RowId rowId, const RowStore &rowStore,
                                   std::span<const Column> schema) {
    const auto *row = rowStore.get(rowId);
    if (row == nullptr) {
        return;
    }
    for (auto &[name, index] : indexes_) {
        if (auto exprIt = indexExpressions_.find(name);
            exprIt != indexExpressions_.end() &&
            exprIt->second.kind == IndexExpression::Kind::Trigram) {
            const auto key = indexKeyForRow(name, *row, schema);
            if (key.isNull() || key.type() != ColumnType::String) {
                continue;
            }
            for (const auto &gram : extractTrigrams(std::get<std::string>(key.data()))) {
                index.insert(Value{gram}, rowId);
            }
            continue;
        }
        index.insert(indexKeyForRow(name, *row, schema), rowId);
    }
    for (auto &[name, index] : orderedIndexes_) {
        if (auto exprIt = indexExpressions_.find(name);
            exprIt != indexExpressions_.end() &&
            exprIt->second.kind == IndexExpression::Kind::Trigram) {
            // Trigram indexes are hash-only; skip ordered maintenance.
            continue;
        }
        index.insert(indexKeyForRow(name, *row, schema), rowId);
    }
}

void IndexManager::rebuildIndexes(const RowStore &rowStore, std::span<const Column> schema) {
    for (auto &[_, index] : indexes_) {
        index.clear();
    }
    for (auto &[_, index] : orderedIndexes_) {
        index.clear();
    }
    for (RowId rowId = 0; rowId < rowStore.capacity(); ++rowId) {
        addRowToIndexes(rowId, rowStore, schema);
    }
}

const std::map<std::string, std::size_t> &IndexManager::indexColumns() const noexcept {
    return indexColumns_;
}

std::map<std::string, std::size_t> &IndexManager::indexColumns() noexcept {
    return indexColumns_;
}

const std::map<std::string, IndexExpression> &IndexManager::indexExpressions() const noexcept {
    return indexExpressions_;
}

std::map<std::string, IndexExpression> &IndexManager::indexExpressions() noexcept {
    return indexExpressions_;
}

const std::map<std::string, HashIndex> &IndexManager::hashIndexes() const noexcept {
    return indexes_;
}

std::map<std::string, HashIndex> &IndexManager::hashIndexes() noexcept { return indexes_; }

const std::map<std::string, BTreeIndex> &IndexManager::orderedIndexes() const noexcept {
    return orderedIndexes_;
}

std::map<std::string, BTreeIndex> &IndexManager::orderedIndexes() noexcept {
    return orderedIndexes_;
}

} // namespace VertexDB
