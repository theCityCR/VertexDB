#include "VertexDB/execution/foreign_key_eval.hpp"

#include <stdexcept>
#include <utility>

namespace VertexDB {
namespace {

const Column *findColumn(std::span<const Column> schema, std::string_view name) {
    for (const auto &column : schema) {
        if (column.name == name) {
            return &column;
        }
    }
    return nullptr;
}

bool parentKeyVisible(Table &parent, std::string_view parentColumn, const Value &value,
                      const ReadSnapshot &snapshot, TransactionManager &transactions) {
    auto hits = parent.indexedLookup(parentColumn, value);
    if (!hits || hits->empty()) {
        // Fallback scan if the constraint index is missing for any reason.
        for (const auto &[rowId, row] : parent.visibleEntries(snapshot, transactions)) {
            const auto index = parent.columnIndex(parentColumn);
            if (!index) {
                return false;
            }
            if (row[*index] == value) {
                return true;
            }
        }
        return false;
    }
    const auto visible = parent.visibleEntriesById(*hits, snapshot, transactions);
    return !visible.empty();
}

[[nodiscard]] std::vector<std::pair<RowId, Row>>
visibleChildrenForKey(Table &child, std::string_view childColumn, const Value &key,
                      const ReadSnapshot &snapshot, TransactionManager &transactions) {
    if (auto hits = child.indexedLookup(childColumn, key)) {
        return child.visibleEntriesById(*hits, snapshot, transactions);
    }
    const auto index = child.columnIndex(childColumn);
    if (!index) {
        return {};
    }
    std::vector<std::pair<RowId, Row>> matches;
    for (const auto &[rowId, row] : child.visibleEntries(snapshot, transactions)) {
        if (!row[*index].isNull() && row[*index] == key) {
            matches.emplace_back(rowId, row);
        }
    }
    return matches;
}

[[nodiscard]] bool isSupportedAction(ForeignKeyAction action) {
    return action == ForeignKeyAction::NoAction || action == ForeignKeyAction::Cascade ||
           action == ForeignKeyAction::SetNull;
}

} // namespace

void validateForeignKeyDefinitions(const Database &database, std::string_view childTableName,
                                   std::span<const Column> childSchema,
                                   std::span<const ForeignKeyConstraint> foreignKeys,
                                   std::span<const Column> creatingSchema) {
    for (const auto &fk : foreignKeys) {
        const auto *childCol = findColumn(childSchema, fk.childColumn);
        if (childCol == nullptr) {
            throw std::invalid_argument("FOREIGN KEY child column not found: " + fk.childColumn);
        }

        std::span<const Column> parentSchema;
        if (fk.parentTable == childTableName && !creatingSchema.empty()) {
            parentSchema = creatingSchema;
        } else {
            auto parent = database.table(fk.parentTable);
            if (!parent) {
                throw std::invalid_argument("FOREIGN KEY parent table not found: " +
                                            fk.parentTable);
            }
            parentSchema = parent->schema();
        }

        const auto *parentCol = findColumn(parentSchema, fk.parentColumn);
        if (parentCol == nullptr) {
            throw std::invalid_argument("FOREIGN KEY parent column not found: " + fk.parentColumn);
        }
        if (!parentCol->unique && !parentCol->primaryKey) {
            throw std::invalid_argument(
                "FOREIGN KEY parent column must be PRIMARY KEY or UNIQUE: " + fk.parentTable +
                "." + fk.parentColumn);
        }
        if (parentCol->type != childCol->type) {
            throw std::invalid_argument("FOREIGN KEY column type mismatch: " + fk.childColumn +
                                        " vs " + fk.parentTable + "." + fk.parentColumn);
        }
        if (!isSupportedAction(fk.onDelete) || !isSupportedAction(fk.onUpdate)) {
            throw std::invalid_argument(
                "FOREIGN KEY only supports ON DELETE/UPDATE NO ACTION, CASCADE, or SET NULL");
        }
        if ((fk.onDelete == ForeignKeyAction::SetNull || fk.onUpdate == ForeignKeyAction::SetNull) &&
            !childCol->nullable) {
            throw std::invalid_argument(
                "FOREIGN KEY SET NULL requires nullable child column: " + fk.childColumn);
        }
    }
}

void assertForeignKeysOnChildRow(Database &database, const Table &child, const Row &row,
                                 const ReadSnapshot &snapshot, TransactionManager &transactions) {
    for (const auto &fk : child.foreignKeys()) {
        const auto childIndex = child.columnIndex(fk.childColumn);
        if (!childIndex) {
            throw std::invalid_argument("FOREIGN KEY child column not found: " + fk.childColumn);
        }
        const Value &value = row[*childIndex];
        if (value.isNull()) {
            continue; // MATCH SIMPLE: NULL child keys are accepted.
        }
        auto parent = database.table(fk.parentTable);
        if (!parent) {
            throw std::invalid_argument("FOREIGN KEY parent table not found: " + fk.parentTable);
        }
        if (!parentKeyVisible(*parent, fk.parentColumn, value, snapshot, transactions)) {
            throw std::invalid_argument("FOREIGN KEY constraint violation on column " +
                                        fk.childColumn);
        }
    }
}

std::vector<ForeignKeyChildHit>
collectReferencingChildren(Database &database, const Table &parent, const Row &parentRow,
                           const ReadSnapshot &snapshot, TransactionManager &transactions,
                           std::optional<std::size_t> updatingColumn, const Value *newValue) {
    std::vector<ForeignKeyChildHit> hits;
    for (const auto &other : database.tables()) {
        for (const auto &fk : other->foreignKeys()) {
            if (fk.parentTable != parent.name()) {
                continue;
            }
            const auto parentIndex = parent.columnIndex(fk.parentColumn);
            if (!parentIndex) {
                continue;
            }
            if (updatingColumn && *updatingColumn != *parentIndex) {
                continue;
            }
            const Value &oldValue = parentRow[*parentIndex];
            if (oldValue.isNull()) {
                continue;
            }
            if (updatingColumn && newValue != nullptr && *newValue == oldValue) {
                continue;
            }
            for (auto &[rowId, row] :
                 visibleChildrenForKey(*other, fk.childColumn, oldValue, snapshot, transactions)) {
                hits.push_back(ForeignKeyChildHit{other, other->name(), fk, rowId, std::move(row)});
            }
        }
    }
    return hits;
}

void assertNoActionParentKeyNotReferenced(std::span<const ForeignKeyChildHit> refs,
                                          bool forUpdate) {
    for (const auto &hit : refs) {
        const auto action = forUpdate ? hit.fk.onUpdate : hit.fk.onDelete;
        if (action == ForeignKeyAction::NoAction) {
            throw std::invalid_argument(
                "FOREIGN KEY constraint violation: key is still referenced");
        }
    }
}

bool tableIsForeignKeyParent(const Database &database, std::string_view tableName) {
    for (const auto &other : database.tables()) {
        for (const auto &fk : other->foreignKeys()) {
            if (fk.parentTable == tableName) {
                return true;
            }
        }
    }
    return false;
}

std::string foreignKeyLiteral(const ForeignKeyConstraint &fk) {
    std::string sql = "FOREIGN KEY (" + fk.childColumn + ") REFERENCES " + fk.parentTable + "(" +
                      fk.parentColumn + ")";
    if (fk.onDelete != ForeignKeyAction::NoAction) {
        sql += " ON DELETE ";
        sql += foreignKeyActionName(fk.onDelete);
    }
    if (fk.onUpdate != ForeignKeyAction::NoAction) {
        sql += " ON UPDATE ";
        sql += foreignKeyActionName(fk.onUpdate);
    }
    return sql;
}

} // namespace VertexDB
