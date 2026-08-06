#include "VertexDB/storage/table.hpp"

#include <algorithm>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>

namespace VertexDB {

Table::Table(std::string name, std::vector<Column> schema)
    : name_(std::move(name)), schema_(std::move(schema)), rowStore_(makePageRowStore()) {
    if (name_.empty()) {
        throw std::invalid_argument("table name cannot be empty");
    }
    if (schema_.empty()) {
        throw std::invalid_argument("table schema cannot be empty");
    }
}

const std::string &Table::name() const noexcept { return name_; }

std::span<const Column> Table::schema() const noexcept { return schema_; }

std::optional<std::size_t> Table::columnIndex(std::string_view column) const {
    auto it =
        std::ranges::find_if(schema_, [column](const Column &item) { return item.name == column; });
    if (it == schema_.end()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(std::distance(schema_.begin(), it));
}

std::vector<Row> Table::rowsSnapshot() const {
    std::shared_lock lock{mutex_};
    return rowStore_->snapshot();
}

std::vector<Row> Table::rowsSnapshot(const ReadSnapshot &snapshot,
                                     const TransactionManager &transactions) const {
    std::shared_lock lock{mutex_};
    return versions_.visibleRows(snapshot, transactions);
}

std::vector<std::pair<RowId, Row>> Table::liveEntries() const {
    std::shared_lock lock{mutex_};
    return rowStore_->liveEntries();
}

std::vector<std::pair<RowId, Row>>
Table::visibleEntries(const ReadSnapshot &snapshot,
                      const TransactionManager &transactions) const {
    std::shared_lock lock{mutex_};
    return versions_.visibleEntries(snapshot, transactions);
}

std::vector<RowId> Table::freeList() const {
    std::shared_lock lock{mutex_};
    return rowStore_->freeList();
}

std::vector<Row> Table::rowsById(std::span<const RowId> rowIds) const {
    std::shared_lock lock{mutex_};
    return rowStore_->rowsById(rowIds);
}

std::vector<Row> Table::rowsById(std::span<const RowId> rowIds, const ReadSnapshot &snapshot,
                                 const TransactionManager &transactions) const {
    std::shared_lock lock{mutex_};
    return versions_.visibleRowsById(rowIds, snapshot, transactions);
}

std::size_t Table::rowCount() const {
    std::shared_lock lock{mutex_};
    return rowStore_->size();
}

std::size_t Table::capacity() const {
    std::shared_lock lock{mutex_};
    return rowStore_->capacity();
}

std::optional<std::vector<RowId>> Table::indexedLookup(std::string_view column,
                                                       const Value &value) const {
    std::shared_lock lock{mutex_};
    for (const auto &[indexName, columnIndex] : indexColumns_) {
        if (schema_[columnIndex].name == column) {
            return indexes_.at(indexName).find(value);
        }
    }
    return std::nullopt;
}

std::optional<std::vector<RowId>>
Table::orderedLookup(std::string_view column, ComparisonOperator op, const Value &value) const {
    std::shared_lock lock{mutex_};
    for (const auto &[indexName, columnIndex] : indexColumns_) {
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

bool Table::hasIndex(std::string_view column) const {
    std::shared_lock lock{mutex_};
    return std::ranges::any_of(
        indexColumns_, [&](const auto &item) { return schema_[item.second].name == column; });
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

std::vector<std::pair<std::string, std::string>> Table::indexDefinitions() const {
    std::shared_lock lock{mutex_};
    std::vector<std::pair<std::string, std::string>> definitions;
    definitions.reserve(indexColumns_.size());
    for (const auto &entry : indexColumns_) {
        definitions.emplace_back(entry.first, schema_.at(entry.second).name);
    }
    return definitions;
}

std::size_t Table::versionCount(RowId rowId) const {
    std::shared_lock lock{mutex_};
    return versions_.versionCount(rowId);
}

RowId Table::insert(Row row, TransactionId writerId) {
    validateRow(row);
    std::unique_lock lock{mutex_};
    const RowId rowId = rowStore_->append(std::move(row));
    versions_.write(rowId, *rowStore_->get(rowId), writerId);
    addRowToIndexes(rowId);
    return rowId;
}

bool Table::erase(RowId rowId, TransactionId writerId) {
    std::unique_lock lock{mutex_};
    if (rowStore_->get(rowId) == nullptr) {
        return false;
    }
    versions_.erase(rowId, writerId);
    const bool erased = rowStore_->erase(rowId);
    if (!erased) {
        return false;
    }
    rebuildIndexes();
    return true;
}

bool Table::update(RowId rowId, std::size_t index, Value value, TransactionId writerId) {
    std::unique_lock lock{mutex_};
    if (rowStore_->get(rowId) == nullptr || index >= schema_.size()) {
        return false;
    }
    if (value.isNull()) {
        if (!schema_[index].nullable) {
            throw std::invalid_argument("null value assigned to non-nullable column");
        }
    } else if (value.type() != schema_[index].type) {
        throw std::invalid_argument("updated value does not match column type");
    }
    auto updated = *rowStore_->get(rowId);
    updated[index] = std::move(value);
    const bool updatedOk = rowStore_->update(rowId, updated);
    if (!updatedOk) {
        return false;
    }
    versions_.write(rowId, *rowStore_->get(rowId), writerId);
    rebuildIndexes();
    return true;
}

bool Table::eraseDiscardingVersion(RowId rowId) {
    std::unique_lock lock{mutex_};
    if (rowStore_->get(rowId) == nullptr) {
        return false;
    }
    if (!rowStore_->erase(rowId)) {
        return false;
    }
    (void)versions_.popLatestVersion(rowId);
    rebuildIndexes();
    return true;
}

bool Table::replaceRow(RowId rowId, Row row) {
    validateRow(row);
    std::unique_lock lock{mutex_};
    if (rowStore_->get(rowId) == nullptr) {
        return false;
    }
    if (!rowStore_->update(rowId, std::move(row))) {
        return false;
    }
    (void)versions_.popLatestVersion(rowId);
    rebuildIndexes();
    return true;
}

bool Table::revive(RowId rowId, Row row) {
    validateRow(row);
    std::unique_lock lock{mutex_};
    if (!rowStore_->revive(rowId, std::move(row))) {
        return false;
    }
    (void)versions_.clearLatestDeletedBy(rowId);
    rebuildIndexes();
    return true;
}

bool Table::applyPhysicalUpsert(RowId rowId, Row row) {
    validateRow(row);
    std::unique_lock lock{mutex_};
    const bool existed = rowStore_->get(rowId) != nullptr;
    if (!rowStore_->upsertAt(rowId, std::move(row))) {
        return false;
    }
    versions_.write(rowId, *rowStore_->get(rowId), kSystemTransactionId);
    if (existed) {
        rebuildIndexes();
    } else {
        addRowToIndexes(rowId);
    }
    return true;
}

bool Table::applyPhysicalErase(RowId rowId) {
    std::unique_lock lock{mutex_};
    if (rowStore_->get(rowId) == nullptr) {
        return false;
    }
    if (!rowStore_->erase(rowId)) {
        return false;
    }
    (void)versions_.popLatestVersion(rowId);
    rebuildIndexes();
    return true;
}

std::optional<Row> Table::getRow(RowId rowId) const {
    std::shared_lock lock{mutex_};
    const auto *row = rowStore_->get(rowId);
    if (row == nullptr) {
        return std::nullopt;
    }
    return *row;
}

bool Table::createIndex(std::string name, std::string column) {
    auto indexColumn = columnIndex(column);
    if (!indexColumn) {
        return false;
    }

    std::unique_lock lock{mutex_};
    if (indexes_.contains(name) || indexColumns_.contains(name)) {
        return false;
    }
    indexColumns_.emplace(name, *indexColumn);
    indexes_.try_emplace(name);
    orderedIndexes_.try_emplace(name);
    // One rebuild path for CREATE INDEX and snapshot restore.
    rebuildIndexes();
    return true;
}

void Table::replaceRows(std::vector<Row> rows) {
    for (const auto &row : rows) {
        validateRow(row);
    }
    std::unique_lock lock{mutex_};
    rowStore_->replaceRows(std::move(rows));
    versions_.clear();
    for (RowId rowId = 0; rowId < rowStore_->capacity(); ++rowId) {
        const auto *row = rowStore_->get(rowId);
        if (row == nullptr) {
            continue;
        }
        versions_.write(rowId, *row, kSystemTransactionId);
    }
    rebuildIndexes();
}

void Table::replaceSparse(std::size_t capacity, std::vector<RowId> freeList,
                          std::vector<std::pair<RowId, Row>> entries) {
    for (const auto &[_, row] : entries) {
        validateRow(row);
    }
    std::unique_lock lock{mutex_};
    rowStore_->replaceSparse(capacity, std::move(freeList), std::move(entries));
    versions_.clear();
    for (RowId rowId = 0; rowId < rowStore_->capacity(); ++rowId) {
        const auto *row = rowStore_->get(rowId);
        if (row == nullptr) {
            continue;
        }
        versions_.write(rowId, *row, kSystemTransactionId);
    }
    rebuildIndexes();
}

void Table::validateRow(const Row &row) const {
    if (row.size() != schema_.size()) {
        throw std::invalid_argument("row width does not match table schema");
    }
    for (std::size_t i = 0; i < row.size(); ++i) {
        if (row[i].isNull()) {
            if (!schema_[i].nullable) {
                throw std::invalid_argument("null value assigned to non-nullable column");
            }
            continue;
        }
        if (row[i].type() != schema_[i].type) {
            throw std::invalid_argument("row value does not match column type");
        }
    }
}

void Table::addRowToIndexes(RowId rowId) {
    const auto *row = rowStore_->get(rowId);
    if (row == nullptr) {
        return;
    }
    for (auto &[name, index] : indexes_) {
        index.insert((*row)[indexColumns_.at(name)], rowId);
    }
    for (auto &[name, index] : orderedIndexes_) {
        index.insert((*row)[indexColumns_.at(name)], rowId);
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
