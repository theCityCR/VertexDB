#include "VertexDB/storage/table.hpp"

#include "VertexDB/storage/page_row_store.hpp"

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
    bool sawPrimaryKey = false;
    for (const auto &column : schema_) {
        if (column.primaryKey) {
            if (sawPrimaryKey) {
                throw std::invalid_argument("multiple PRIMARY KEY columns are not supported");
            }
            if (column.nullable) {
                throw std::invalid_argument("PRIMARY KEY column cannot be NULL");
            }
            if (!column.unique) {
                throw std::invalid_argument("PRIMARY KEY column must be UNIQUE");
            }
            sawPrimaryKey = true;
        }
    }
}

const std::string &Table::name() const noexcept { return name_; }

void Table::setName(std::string name) {
    std::unique_lock lock{mutex_};
    if (name.empty()) {
        throw std::invalid_argument("table name cannot be empty");
    }
    name_ = std::move(name);
}

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
                                     TransactionManager &transactions) const {
    std::shared_lock lock{mutex_};
    auto entries = versions_.visibleEntries(snapshot, transactions);
    for (const auto &[rowId, _] : entries) {
        transactions.recordRead(snapshot.self, name_, rowId);
    }
    std::vector<Row> rows;
    rows.reserve(entries.size());
    for (auto &[_, row] : entries) {
        rows.push_back(std::move(row));
    }
    return rows;
}

std::vector<std::pair<RowId, Row>> Table::liveEntries() const {
    std::shared_lock lock{mutex_};
    return rowStore_->liveEntries();
}

std::vector<std::pair<RowId, Row>>
Table::visibleEntries(const ReadSnapshot &snapshot, TransactionManager &transactions) const {
    std::shared_lock lock{mutex_};
    auto entries = versions_.visibleEntries(snapshot, transactions);
    for (const auto &[rowId, _] : entries) {
        transactions.recordRead(snapshot.self, name_, rowId);
    }
    return entries;
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
                                 TransactionManager &transactions) const {
    std::shared_lock lock{mutex_};
    auto entries = versions_.visibleEntriesById(rowIds, snapshot, transactions);
    for (const auto &[rowId, _] : entries) {
        transactions.recordRead(snapshot.self, name_, rowId);
    }
    std::vector<Row> rows;
    rows.reserve(entries.size());
    for (auto &[_, row] : entries) {
        rows.push_back(std::move(row));
    }
    return rows;
}

std::vector<std::pair<RowId, Row>>
Table::visibleEntriesById(std::span<const RowId> rowIds, const ReadSnapshot &snapshot,
                          TransactionManager &transactions) const {
    std::shared_lock lock{mutex_};
    auto entries = versions_.visibleEntriesById(rowIds, snapshot, transactions);
    for (const auto &[rowId, _] : entries) {
        transactions.recordRead(snapshot.self, name_, rowId);
    }
    return entries;
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

RowId Table::insert(Row row, TransactionId writerId, TransactionManager *transactions) {
    validateRow(row);
    std::unique_lock lock{mutex_};
    enforceUniqueConstraintsUnlocked(row, std::nullopt);
    const RowId rowId = rowStore_->append(std::move(row));
    const Row &stored = *rowStore_->get(rowId);
    versions_.write(rowId, stored, writerId);
    addRowToIndexes(rowId);
    if (transactions != nullptr) {
        transactions->recordWrite(writerId, name_, rowId);
        SsiInsert image;
        image.relation = name_;
        image.columns.reserve(schema_.size());
        for (std::size_t i = 0; i < schema_.size(); ++i) {
            image.columns.emplace_back(schema_[i].name, stored[i]);
        }
        transactions->recordInsert(writerId, std::move(image));
    }
    return rowId;
}

bool Table::erase(RowId rowId, TransactionId writerId, TransactionManager *transactions) {
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
    if (transactions != nullptr) {
        transactions->recordWrite(writerId, name_, rowId);
    }
    return true;
}

bool Table::update(RowId rowId, std::size_t index, Value value, TransactionId writerId,
                   TransactionManager *transactions) {
    std::unique_lock lock{mutex_};
    if (rowStore_->get(rowId) == nullptr || index >= schema_.size()) {
        return false;
    }
    if (value.isNull()) {
        if (!schema_[index].nullable) {
            throw std::invalid_argument("NOT NULL constraint violation on column " +
                                        schema_[index].name);
        }
    } else if (value.type() != schema_[index].type) {
        throw std::invalid_argument("updated value does not match column type");
    }
    auto updated = *rowStore_->get(rowId);
    updated[index] = std::move(value);
    enforceUniqueConstraintsUnlocked(updated, rowId);
    const bool updatedOk = rowStore_->update(rowId, updated);
    if (!updatedOk) {
        return false;
    }
    const Row &stored = *rowStore_->get(rowId);
    versions_.write(rowId, stored, writerId);
    rebuildIndexes();
    if (transactions != nullptr) {
        transactions->recordWrite(writerId, name_, rowId);
        // Update-into-predicate is treated like an insert for phantom SSI matching.
        SsiInsert image;
        image.relation = name_;
        image.columns.reserve(schema_.size());
        for (std::size_t i = 0; i < schema_.size(); ++i) {
            image.columns.emplace_back(schema_[i].name, stored[i]);
        }
        transactions->recordInsert(writerId, std::move(image));
    }
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
    enforceUniqueConstraintsUnlocked(row, rowId);
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
    enforceUniqueConstraintsUnlocked(row, rowId);
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
                throw std::invalid_argument("NOT NULL constraint violation on column " +
                                            schema_[i].name);
            }
            continue;
        }
        if (row[i].type() != schema_[i].type) {
            throw std::invalid_argument("row value does not match column type");
        }
    }
}

void Table::assertUniqueRow(const Row &row, std::optional<RowId> excludeRowId) const {
    validateRow(row);
    std::shared_lock lock{mutex_};
    enforceUniqueConstraintsUnlocked(row, excludeRowId);
}

void Table::ensureConstraintIndexes() {
    std::unique_lock lock{mutex_};
    for (std::size_t i = 0; i < schema_.size(); ++i) {
        const auto &column = schema_[i];
        if (!column.unique && !column.primaryKey) {
            continue;
        }
        if (indexManager_.hasIndex(column.name, schema_)) {
            continue;
        }
        const std::string indexName =
            (column.primaryKey ? "__pk_" : "__uq_") + column.name;
        if (!registerIndex(indexName, i, std::nullopt, true)) {
            throw std::runtime_error("failed to create constraint index " + indexName);
        }
    }
}

void Table::enforceUniqueConstraintsUnlocked(const Row &row,
                                             std::optional<RowId> excludeRowId) const {
    for (std::size_t i = 0; i < schema_.size(); ++i) {
        if (!schema_[i].unique && !schema_[i].primaryKey) {
            continue;
        }
        const Value &value = row[i];
        if (value.isNull()) {
            // UNIQUE allows multiple NULLs; PRIMARY KEY is non-nullable.
            continue;
        }
        if (auto hits = indexManager_.indexedLookup(schema_[i].name, value, schema_)) {
            for (const RowId hit : *hits) {
                if (excludeRowId && hit == *excludeRowId) {
                    continue;
                }
                throw std::invalid_argument("unique constraint violation on column " +
                                            schema_[i].name);
            }
            continue;
        }
        for (const auto &[rowId, existing] : rowStore_->liveEntries()) {
            if (excludeRowId && rowId == *excludeRowId) {
                continue;
            }
            if (existing[i] == value) {
                throw std::invalid_argument("unique constraint violation on column " +
                                            schema_[i].name);
            }
        }
    }
}

} // namespace VertexDB
