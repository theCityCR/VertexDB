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

std::optional<Row> Table::getRow(RowId rowId) const {
    std::shared_lock lock{mutex_};
    const auto *row = rowStore_->get(rowId);
    if (row == nullptr) {
        return std::nullopt;
    }
    return *row;
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

} // namespace VertexDB
