#include "VertexDB/storage/table.hpp"

#include "VertexDB/execution/foreign_key_eval.hpp"
#include "VertexDB/storage/database.hpp"

#include <algorithm>
#include <map>
#include <mutex>
#include <stdexcept>
#include <type_traits>
#include <unordered_set>
#include <utility>

namespace VertexDB {
namespace {

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

void renameCheckColumn(Predicate &predicate, std::string_view oldName, std::string_view newName) {
    std::visit(
        [&](auto &node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, AndPred> || std::is_same_v<T, OrPred>) {
                renameCheckColumn(*node.left, oldName, newName);
                renameCheckColumn(*node.right, oldName, newName);
            } else if constexpr (std::is_same_v<T, ComparisonPred>) {
                if (node.column == oldName) {
                    node.column = std::string{newName};
                }
                if (node.rhsColumn && *node.rhsColumn == oldName) {
                    *node.rhsColumn = std::string{newName};
                }
            } else if constexpr (std::is_same_v<T, InListPred> || std::is_same_v<T, LikePred> ||
                                 std::is_same_v<T, RegexPred> ||
                                 std::is_same_v<T, InSubqueryPred>) {
                if (node.column == oldName) {
                    node.column = std::string{newName};
                }
            }
        },
        predicate);
}

[[nodiscard]] bool indexReferencesColumn(const IndexDefinition &definition,
                                         std::string_view columnName) {
    if (definition.expression) {
        return definition.expression->column == columnName;
    }
    return std::ranges::any_of(definition.columns,
                               [&](const std::string &name) { return name == columnName; });
}

[[nodiscard]] bool uniqueReferencesColumn(const UniqueConstraint &constraint,
                                          std::string_view columnName) {
    return std::ranges::any_of(constraint.columns,
                               [&](const std::string &name) { return name == columnName; });
}

[[nodiscard]] bool foreignKeyReferencesColumn(const ForeignKeyConstraint &fk,
                                              std::string_view columnName) {
    return std::ranges::any_of(fk.childColumns,
                               [&](const std::string &name) { return name == columnName; });
}

void renameNameList(std::vector<std::string> &names, std::string_view oldName,
                    std::string_view newName) {
    for (auto &name : names) {
        if (name == oldName) {
            name = std::string{newName};
        }
    }
}

} // namespace

void Table::addColumn(Column column, std::optional<Value> fill) {
    if (column.unique || column.primaryKey) {
        throw std::invalid_argument(
            "ALTER TABLE ADD COLUMN does not support UNIQUE / PRIMARY KEY");
    }
    if (column.name.empty()) {
        throw std::invalid_argument("column name cannot be empty");
    }
    if (fill) {
        if (fill->isNull()) {
            if (!column.nullable) {
                throw std::invalid_argument("DEFAULT NULL requires a nullable column");
            }
        } else if (fill->type() != column.type) {
            throw std::invalid_argument("DEFAULT literal type does not match column type");
        }
    } else if (!column.nullable) {
        std::unique_lock lock{mutex_};
        if (columnIndex(column.name)) {
            throw std::invalid_argument("column already exists: " + column.name);
        }
        if (!rowStore_->liveEntries().empty()) {
            throw std::invalid_argument(
                "ALTER TABLE ADD COLUMN NOT NULL requires DEFAULT when the table has rows");
        }
        schema_.push_back(std::move(column));
        versions_.transformRows([](Row &row) { row.push_back(Value{}); });
        return;
    }

    const Value pad = fill.value_or(Value{});
    std::unique_lock lock{mutex_};
    if (columnIndex(column.name)) {
        throw std::invalid_argument("column already exists: " + column.name);
    }
    schema_.push_back(std::move(column));
    for (auto &[rowId, row] : rowStore_->liveEntries()) {
        Row widened = row;
        widened.push_back(pad);
        if (!rowStore_->update(rowId, std::move(widened))) {
            throw std::runtime_error("failed to pad row during ADD COLUMN");
        }
    }
    versions_.transformRows([&](Row &row) { row.push_back(pad); });
}

Table::DroppedColumnCapture Table::dropColumn(std::string_view columnName, const Database *database,
                                              bool cascade) {
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

    if (database != nullptr && columnIsForeignKeyParent(*database, name_, columnName)) {
        throw std::invalid_argument("cannot DROP COLUMN: column is referenced by FOREIGN KEY");
    }

    DroppedColumnCapture capture;
    capture.column = column;
    capture.columnIndex = index;
    capture.cascaded = cascade;

    auto rejectOrCascade = [&](bool dependent, const char *message) {
        if (!dependent) {
            return;
        }
        if (!cascade) {
            throw std::invalid_argument(message);
        }
    };

    // User indexes (constraint indexes handled with UNIQUE/PK below).
    for (const auto &definition : indexManager_.indexDefinitions(schema_)) {
        if (definition.name.starts_with("__pk_") || definition.name.starts_with("__uq_")) {
            continue;
        }
        if (!indexReferencesColumn(definition, columnName)) {
            continue;
        }
        rejectOrCascade(true, "cannot DROP COLUMN: column is indexed");
        capture.droppedUserIndexes.push_back(definition);
    }

    for (const auto &check : checkConstraints_) {
        if (!checkReferencesColumn(check, columnName)) {
            continue;
        }
        rejectOrCascade(true, "cannot DROP COLUMN: column referenced by CHECK");
        capture.droppedChecks.push_back(check);
    }

    if (columnIsForeignKeyChild(*this, columnName)) {
        rejectOrCascade(true, "cannot DROP COLUMN: column is a FOREIGN KEY child");
        for (const auto &fk : foreignKeys_) {
            if (foreignKeyReferencesColumn(fk, columnName)) {
                capture.droppedChildForeignKeys.push_back(fk);
            }
        }
    }

    if (column.primaryKey || column.unique) {
        rejectOrCascade(true, "cannot DROP COLUMN: column is PRIMARY KEY or UNIQUE");
    }
    for (const auto &constraint : uniqueConstraints_) {
        if (!uniqueReferencesColumn(constraint, columnName)) {
            continue;
        }
        rejectOrCascade(true,
                        "cannot DROP COLUMN: column is part of a UNIQUE / PRIMARY KEY constraint");
        capture.droppedUniques.push_back(constraint);
    }
    // Column-level UNIQUE/PK is restored via capture.column flags; also track for index drop.
    if (column.primaryKey || column.unique) {
        const bool covered = std::ranges::any_of(
            capture.droppedUniques, [&](const UniqueConstraint &c) {
                return c.columns.size() == 1 && c.columns.front() == columnName;
            });
        if (!covered) {
            capture.droppedUniques.push_back(
                UniqueConstraint{{std::string{columnName}}, column.primaryKey});
        }
    }

    if (cascade) {
        for (const auto &definition : capture.droppedUserIndexes) {
            if (!indexManager_.dropIndex(definition.name)) {
                throw std::runtime_error("failed to CASCADE-drop index " + definition.name);
            }
        }
        std::erase_if(checkConstraints_, [&](const Predicate &check) {
            return checkReferencesColumn(check, columnName);
        });
        std::erase_if(foreignKeys_, [&](const ForeignKeyConstraint &fk) {
            return foreignKeyReferencesColumn(fk, columnName);
        });
        for (const auto &constraint : capture.droppedUniques) {
            const std::string indexName = constraintIndexName(constraint);
            (void)indexManager_.dropIndex(indexName);
        }
        std::erase_if(uniqueConstraints_, [&](const UniqueConstraint &constraint) {
            return uniqueReferencesColumn(constraint, columnName);
        });
    }

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

    // Column indexes stored in IndexManager are positions; shift after erase.
    for (auto &[_, columnIndexes] : indexManager_.indexColumns()) {
        for (auto &columnIndex : columnIndexes) {
            if (columnIndex == index) {
                throw std::runtime_error("index still references dropped column");
            }
            if (columnIndex > index) {
                --columnIndex;
            }
        }
    }
    indexManager_.rebuildIndexes(*rowStore_, schema_);

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
    // Shift remaining index column positions after insert.
    for (auto &[_, columnIndexes] : indexManager_.indexColumns()) {
        for (auto &columnIndex : columnIndexes) {
            if (columnIndex >= capture.columnIndex) {
                ++columnIndex;
            }
        }
    }
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

    if (capture.cascaded) {
        for (const auto &check : capture.droppedChecks) {
            checkConstraints_.push_back(check);
        }
        for (const auto &fk : capture.droppedChildForeignKeys) {
            foreignKeys_.push_back(fk);
        }
        for (const auto &constraint : capture.droppedUniques) {
            const bool columnLevel =
                constraint.columns.size() == 1 && constraint.columns.front() == capture.column.name;
            if (columnLevel) {
                // Flags live on capture.column already restored into schema_.
                continue;
            }
            uniqueConstraints_.push_back(constraint);
        }
        for (const auto &definition : capture.droppedUserIndexes) {
            std::vector<std::size_t> indexes;
            if (definition.expression) {
                const auto index = columnIndex(definition.expression->column);
                if (!index) {
                    throw std::runtime_error("failed to restore cascaded index " + definition.name);
                }
                indexes.push_back(*index);
                if (!registerIndex(definition.name, std::move(indexes), definition.expression,
                                   false)) {
                    throw std::runtime_error("failed to restore cascaded index " + definition.name);
                }
            } else {
                for (const auto &columnName : definition.columns) {
                    const auto index = columnIndex(columnName);
                    if (!index) {
                        throw std::runtime_error("failed to restore cascaded index " +
                                                 definition.name);
                    }
                    indexes.push_back(*index);
                }
                if (!registerIndex(definition.name, std::move(indexes), std::nullopt, false)) {
                    throw std::runtime_error("failed to restore cascaded index " + definition.name);
                }
            }
        }
        for (const auto &constraint : allUniqueConstraints()) {
            if (indexManager_.hasIndex(constraint.columns, schema_)) {
                continue;
            }
            const std::string indexName = constraintIndexName(constraint);
            std::vector<std::size_t> indexes;
            for (const auto &columnName : constraint.columns) {
                const auto index = columnIndex(columnName);
                if (!index) {
                    throw std::runtime_error("failed to restore constraint index " + indexName);
                }
                indexes.push_back(*index);
            }
            if (!registerIndex(indexName, std::move(indexes), std::nullopt, false)) {
                throw std::runtime_error("failed to restore constraint index " + indexName);
            }
        }
    }

    indexManager_.rebuildIndexes(*rowStore_, schema_);
}

void Table::rewriteForeignKeyParentColumn(std::string_view parentTable, std::string_view oldName,
                                          std::string_view newName) {
    std::unique_lock lock{mutex_};
    for (auto &fk : foreignKeys_) {
        if (fk.parentTable != parentTable) {
            continue;
        }
        renameNameList(fk.parentColumns, oldName, newName);
    }
}

void Table::renameColumn(std::string_view oldName, std::string_view newName, Database *database) {
    if (oldName.empty() || newName.empty()) {
        throw std::invalid_argument("column name cannot be empty");
    }
    if (oldName == newName) {
        return;
    }
    std::unique_lock lock{mutex_};
    const auto indexOpt = columnIndex(oldName);
    if (!indexOpt) {
        throw std::invalid_argument("unknown column: " + std::string{oldName});
    }
    if (columnIndex(newName)) {
        throw std::invalid_argument("column already exists: " + std::string{newName});
    }

    // Capture constraint index names before renaming columns.
    std::vector<std::pair<std::string, UniqueConstraint>> constraintRenames;
    for (const auto &constraint : allUniqueConstraints()) {
        if (!uniqueReferencesColumn(constraint, oldName)) {
            continue;
        }
        UniqueConstraint updated = constraint;
        renameNameList(updated.columns, oldName, newName);
        constraintRenames.emplace_back(constraintIndexName(constraint), updated);
    }

    schema_[*indexOpt].name = std::string{newName};

    for (auto &[name, expression] : indexManager_.indexExpressions()) {
        if (expression.column == oldName) {
            expression.column = std::string{newName};
        }
    }

    for (auto &check : checkConstraints_) {
        renameCheckColumn(check, oldName, newName);
    }

    for (auto &constraint : uniqueConstraints_) {
        renameNameList(constraint.columns, oldName, newName);
    }
    for (auto &fk : foreignKeys_) {
        renameNameList(fk.childColumns, oldName, newName);
    }

    auto histograms = statistics_.columnHistograms();
    for (auto &histogram : histograms) {
        if (histogram.column == oldName) {
            histogram.column = std::string{newName};
        }
    }
    statistics_.replaceColumnHistograms(std::move(histograms));

    for (const auto &[oldIndexName, updated] : constraintRenames) {
        const std::string newIndexName = constraintIndexName(updated);
        if (oldIndexName == newIndexName) {
            continue;
        }
        if (!indexManager_.dropIndex(oldIndexName)) {
            // May already be absent if only column-level flags without dedicated index yet.
            continue;
        }
        std::vector<std::size_t> indexes;
        for (const auto &columnName : updated.columns) {
            const auto index = columnIndex(columnName);
            if (!index) {
                throw std::runtime_error("failed to rename constraint index");
            }
            indexes.push_back(*index);
        }
        if (!registerIndex(newIndexName, std::move(indexes), std::nullopt, true)) {
            throw std::runtime_error("failed to recreate constraint index " + newIndexName);
        }
    }

    // Release lock before touching other tables (avoid lock-order issues).
    lock.unlock();
    if (database != nullptr) {
        for (const auto &other : database->tables()) {
            if (other.get() == this) {
                continue;
            }
            other->rewriteForeignKeyParentColumn(name_, oldName, newName);
        }
    }
}

} // namespace VertexDB
