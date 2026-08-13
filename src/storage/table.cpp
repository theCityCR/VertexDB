#include "VertexDB/storage/table.hpp"

#include "VertexDB/execution/foreign_key_eval.hpp"
#include "VertexDB/storage/check_eval.hpp"
#include "VertexDB/storage/database.hpp"
#include "VertexDB/storage/page_row_store.hpp"

#include <algorithm>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <type_traits>
#include <unordered_set>
#include <utility>

namespace VertexDB {
namespace {

void validateUniqueConstraintColumns(const UniqueConstraint &constraint,
                                     std::span<const Column> schema) {
    if (constraint.columns.empty()) {
        throw std::invalid_argument("UNIQUE / PRIMARY KEY requires at least one column");
    }
    std::unordered_set<std::string> seen;
    for (const auto &name : constraint.columns) {
        if (!seen.insert(name).second) {
            throw std::invalid_argument("duplicate column in UNIQUE / PRIMARY KEY: " + name);
        }
        const auto it = std::ranges::find_if(
            schema, [&](const Column &column) { return column.name == name; });
        if (it == schema.end()) {
            throw std::invalid_argument("unknown UNIQUE / PRIMARY KEY column: " + name);
        }
        if (constraint.primaryKey && it->nullable) {
            throw std::invalid_argument("PRIMARY KEY column cannot be NULL: " + name);
        }
    }
}

void collectCheckColumns(const Predicate &predicate, std::unordered_set<std::string> &columns) {
    std::visit(
        [&](const auto &node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, AndPred> || std::is_same_v<T, OrPred>) {
                collectCheckColumns(*node.left, columns);
                collectCheckColumns(*node.right, columns);
            } else if constexpr (std::is_same_v<T, ComparisonPred>) {
                columns.insert(node.column);
                if (node.rhsColumn) {
                    columns.insert(*node.rhsColumn);
                }
            }
        },
        predicate);
}

bool checkReferencesColumn(const Predicate &predicate, std::string_view columnName) {
    std::unordered_set<std::string> columns;
    collectCheckColumns(predicate, columns);
    return columns.contains(std::string{columnName});
}

} // namespace

Table::Table(std::string name, std::vector<Column> schema,
             std::vector<Predicate> checkConstraints,
             std::vector<ForeignKeyConstraint> foreignKeys,
             std::vector<UniqueConstraint> uniqueConstraints)
    : name_(std::move(name)), schema_(std::move(schema)),
      checkConstraints_(std::move(checkConstraints)), foreignKeys_(std::move(foreignKeys)),
      uniqueConstraints_(std::move(uniqueConstraints)), rowStore_(makePageRowStore()) {
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
    for (const auto &constraint : uniqueConstraints_) {
        validateUniqueConstraintColumns(constraint, schema_);
        if (constraint.primaryKey) {
            if (sawPrimaryKey) {
                throw std::invalid_argument("multiple PRIMARY KEY constraints are not supported");
            }
            sawPrimaryKey = true;
            for (auto &column : schema_) {
                if (std::ranges::find(constraint.columns, column.name) != constraint.columns.end()) {
                    column.nullable = false;
                }
            }
        }
    }
    for (const auto &check : checkConstraints_) {
        assertSimpleCheckConstraint(check);
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

std::span<const Predicate> Table::checkConstraints() const noexcept { return checkConstraints_; }

std::span<const ForeignKeyConstraint> Table::foreignKeys() const noexcept { return foreignKeys_; }

std::span<const UniqueConstraint> Table::uniqueConstraints() const noexcept {
    return uniqueConstraints_;
}

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
    enforceCheckConstraints(updated);
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
    enforceCheckConstraints(row);
}

void Table::enforceCheckConstraints(const Row &row) const {
    auto lookup = [this](std::string_view column) -> std::optional<std::size_t> {
        return columnIndex(column);
    };
    for (const auto &check : checkConstraints_) {
        if (!evalCheckPredicate(check, row, lookup)) {
            throw std::invalid_argument("CHECK constraint violation: " +
                                        checkConstraintLiteral(check));
        }
    }
}

void Table::assertUniqueRow(const Row &row, std::optional<RowId> excludeRowId) const {
    validateRow(row);
    std::shared_lock lock{mutex_};
    enforceUniqueConstraintsUnlocked(row, excludeRowId);
}

bool Table::rowsConflictOnUnique(const Row &left, const Row &right) const {
    for (const auto &constraint : allUniqueConstraints()) {
        if (uniqueRowsEqual(constraint, left, right)) {
            return true;
        }
    }
    return false;
}

std::vector<UniqueConstraint> Table::allUniqueConstraints() const {
    std::vector<UniqueConstraint> constraints = uniqueConstraints_;
    for (const auto &column : schema_) {
        if (!column.unique && !column.primaryKey) {
            continue;
        }
        // Column-level flags already covered when a matching table-level constraint exists.
        const bool covered = std::ranges::any_of(uniqueConstraints_, [&](const UniqueConstraint &c) {
            return c.columns.size() == 1 && c.columns.front() == column.name;
        });
        if (covered) {
            continue;
        }
        constraints.push_back(UniqueConstraint{{column.name}, column.primaryKey});
    }
    return constraints;
}

std::string Table::constraintIndexName(const UniqueConstraint &constraint) {
    std::string name = constraint.primaryKey ? "__pk_" : "__uq_";
    for (std::size_t i = 0; i < constraint.columns.size(); ++i) {
        if (i != 0) {
            name.push_back('_');
        }
        name += constraint.columns[i];
    }
    return name;
}

std::string Table::formatUniqueColumns(const UniqueConstraint &constraint) {
    if (constraint.columns.size() == 1) {
        return constraint.columns.front();
    }
    std::string text = "(";
    for (std::size_t i = 0; i < constraint.columns.size(); ++i) {
        if (i != 0) {
            text += ", ";
        }
        text += constraint.columns[i];
    }
    text.push_back(')');
    return text;
}

Value Table::uniqueKeyForRow(const UniqueConstraint &constraint, const Row &row) const {
    std::vector<Value> parts;
    parts.reserve(constraint.columns.size());
    for (const auto &columnName : constraint.columns) {
        const auto index = columnIndex(columnName);
        if (!index) {
            throw std::runtime_error("unknown unique constraint column");
        }
        parts.push_back(row[*index]);
    }
    return Value::composite(std::move(parts));
}

bool Table::uniqueRowsEqual(const UniqueConstraint &constraint, const Row &left,
                            const Row &right) const {
    for (const auto &columnName : constraint.columns) {
        const auto index = columnIndex(columnName);
        if (!index) {
            return false;
        }
        if (left[*index].isNull() || right[*index].isNull()) {
            return false;
        }
        if (!(left[*index] == right[*index])) {
            return false;
        }
    }
    return true;
}

void Table::ensureConstraintIndexes() {
    std::unique_lock lock{mutex_};
    for (const auto &constraint : allUniqueConstraints()) {
        if (indexManager_.hasIndex(constraint.columns, schema_)) {
            continue;
        }
        const std::string indexName = constraintIndexName(constraint);
        std::vector<std::size_t> indexes;
        indexes.reserve(constraint.columns.size());
        for (const auto &columnName : constraint.columns) {
            const auto index = columnIndex(columnName);
            if (!index) {
                throw std::runtime_error("failed to create constraint index " + indexName);
            }
            indexes.push_back(*index);
        }
        if (!registerIndex(indexName, std::move(indexes), std::nullopt, true)) {
            throw std::runtime_error("failed to create constraint index " + indexName);
        }
    }
}

void Table::enforceUniqueConstraintsUnlocked(const Row &row,
                                             std::optional<RowId> excludeRowId) const {
    for (const auto &constraint : allUniqueConstraints()) {
        const Value key = uniqueKeyForRow(constraint, row);
        if (key.hasNullCompositePart()) {
            // UNIQUE allows rows with NULL parts; PRIMARY KEY columns are non-nullable.
            continue;
        }
        if (auto hits = indexManager_.indexedLookup(constraint.columns, key, schema_)) {
            for (const RowId hit : *hits) {
                if (excludeRowId && hit == *excludeRowId) {
                    continue;
                }
                throw std::invalid_argument("unique constraint violation on column " +
                                            formatUniqueColumns(constraint));
            }
            continue;
        }
        for (const auto &[rowId, existing] : rowStore_->liveEntries()) {
            if (excludeRowId && rowId == *excludeRowId) {
                continue;
            }
            if (uniqueRowsEqual(constraint, row, existing)) {
                throw std::invalid_argument("unique constraint violation on column " +
                                            formatUniqueColumns(constraint));
            }
        }
    }
}

void Table::addNullableColumn(Column column) {
    if (!column.nullable || column.unique || column.primaryKey) {
        throw std::invalid_argument(
            "ALTER TABLE ADD COLUMN requires a nullable column without UNIQUE / PRIMARY KEY");
    }
    if (column.name.empty()) {
        throw std::invalid_argument("column name cannot be empty");
    }
    std::unique_lock lock{mutex_};
    if (columnIndex(column.name)) {
        throw std::invalid_argument("column already exists: " + column.name);
    }
    schema_.push_back(std::move(column));
    for (auto &[rowId, row] : rowStore_->liveEntries()) {
        Row widened = row;
        widened.push_back(Value{});
        if (!rowStore_->update(rowId, std::move(widened))) {
            throw std::runtime_error("failed to pad row during ADD COLUMN");
        }
    }
    versions_.transformRows([](Row &row) { row.push_back(Value{}); });
}

Table::DroppedColumnCapture Table::dropUnreferencedColumn(std::string_view columnName,
                                                          const Database *database) {
    std::unique_lock lock{mutex_};
    const auto indexOpt = columnIndex(columnName);
    if (!indexOpt) {
        throw std::invalid_argument("unknown column: " + std::string{columnName});
    }
    if (schema_.size() <= 1) {
        throw std::invalid_argument("cannot DROP the last column of a table");
    }
    const std::size_t index = *indexOpt;
    const Column &column = schema_[index];
    for (const auto &definition : indexManager_.indexDefinitions(schema_)) {
        if (definition.name.starts_with("__pk_") || definition.name.starts_with("__uq_")) {
            continue; // Constraint indexes are covered by UNIQUE / PRIMARY KEY checks below.
        }
        if (definition.expression) {
            if (definition.expression->column == columnName) {
                throw std::invalid_argument("cannot DROP COLUMN: column is indexed");
            }
        } else {
            for (const auto &name : definition.columns) {
                if (name == columnName) {
                    throw std::invalid_argument("cannot DROP COLUMN: column is indexed");
                }
            }
        }
    }
    for (const auto &check : checkConstraints_) {
        if (checkReferencesColumn(check, columnName)) {
            throw std::invalid_argument("cannot DROP COLUMN: column referenced by CHECK");
        }
    }
    if (columnIsForeignKeyChild(*this, columnName)) {
        throw std::invalid_argument("cannot DROP COLUMN: column is a FOREIGN KEY child");
    }
    if (database != nullptr && columnIsForeignKeyParent(*database, name_, columnName)) {
        throw std::invalid_argument("cannot DROP COLUMN: column is referenced by FOREIGN KEY");
    }
    if (column.primaryKey || column.unique) {
        throw std::invalid_argument("cannot DROP COLUMN: column is PRIMARY KEY or UNIQUE");
    }
    for (const auto &constraint : uniqueConstraints_) {
        for (const auto &name : constraint.columns) {
            if (name == columnName) {
                throw std::invalid_argument(
                    "cannot DROP COLUMN: column is part of a UNIQUE / PRIMARY KEY constraint");
            }
        }
    }

    DroppedColumnCapture capture;
    capture.column = column;
    capture.columnIndex = index;
    for (auto &[rowId, row] : rowStore_->liveEntries()) {
        if (index >= row.size()) {
            throw std::runtime_error("heap row narrower than schema during DROP COLUMN");
        }
        capture.heapValues.emplace_back(rowId, row[index]);
        Row narrowed = row;
        narrowed.erase(narrowed.begin() + static_cast<std::ptrdiff_t>(index));
        if (!rowStore_->update(rowId, std::move(narrowed))) {
            throw std::runtime_error("failed to rewrite row during DROP COLUMN");
        }
    }
    capture.versionValues = versions_.extractColumn(index);
    schema_.erase(schema_.begin() + static_cast<std::ptrdiff_t>(index));

    auto histograms = statistics_.columnHistograms();
    std::erase_if(histograms, [&](const ColumnHistogram &histogram) {
        return histogram.column == columnName;
    });
    statistics_.replaceColumnHistograms(std::move(histograms));
    return capture;
}

void Table::restoreDroppedColumn(const DroppedColumnCapture &capture) {
    std::unique_lock lock{mutex_};
    if (capture.columnIndex > schema_.size()) {
        throw std::runtime_error("ALTER DROP COLUMN undo index out of range");
    }
    if (columnIndex(capture.column.name)) {
        throw std::runtime_error("ALTER DROP COLUMN undo column already exists");
    }
    schema_.insert(schema_.begin() + static_cast<std::ptrdiff_t>(capture.columnIndex),
                   capture.column);
    versions_.insertColumn(capture.columnIndex, capture.versionValues);

    std::map<RowId, Value> heapById;
    for (const auto &[rowId, value] : capture.heapValues) {
        heapById.emplace(rowId, value);
    }
    for (auto &[rowId, row] : rowStore_->liveEntries()) {
        const auto it = heapById.find(rowId);
        if (it == heapById.end()) {
            throw std::runtime_error("ALTER DROP COLUMN undo missing heap value");
        }
        Row widened = row;
        if (capture.columnIndex > widened.size()) {
            throw std::runtime_error("ALTER DROP COLUMN undo heap index out of range");
        }
        widened.insert(widened.begin() + static_cast<std::ptrdiff_t>(capture.columnIndex),
                       it->second);
        if (!rowStore_->update(rowId, std::move(widened))) {
            throw std::runtime_error("failed to restore row during DROP COLUMN undo");
        }
    }
}

} // namespace VertexDB
