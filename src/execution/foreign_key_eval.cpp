#include "VertexDB/execution/foreign_key_eval.hpp"

#include <algorithm>
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

[[nodiscard]] bool columnsEqual(std::span<const std::string> left,
                                std::span<const std::string> right) {
    return left.size() == right.size() &&
           std::equal(left.begin(), left.end(), right.begin());
}

[[nodiscard]] std::vector<UniqueConstraint>
uniqueConstraintsFromSchema(std::span<const Column> schema,
                            std::span<const UniqueConstraint> tableLevel) {
    std::vector<UniqueConstraint> constraints{tableLevel.begin(), tableLevel.end()};
    for (const auto &column : schema) {
        if (!column.unique && !column.primaryKey) {
            continue;
        }
        const bool covered = std::ranges::any_of(constraints, [&](const UniqueConstraint &c) {
            return c.columns.size() == 1 && c.columns.front() == column.name;
        });
        if (!covered) {
            constraints.push_back(
                UniqueConstraint{{column.name}, column.primaryKey});
        }
    }
    return constraints;
}

[[nodiscard]] bool parentHasExactUniqueKey(std::span<const UniqueConstraint> constraints,
                                           std::span<const std::string> parentColumns) {
    return std::ranges::any_of(constraints, [&](const UniqueConstraint &constraint) {
        return columnsEqual(constraint.columns, parentColumns);
    });
}

[[nodiscard]] std::optional<Value> keyFromRow(const Table &table, const Row &row,
                                              std::span<const std::string> columns) {
    std::vector<Value> parts;
    parts.reserve(columns.size());
    for (const auto &column : columns) {
        const auto index = table.columnIndex(column);
        if (!index) {
            return std::nullopt;
        }
        parts.push_back(row[*index]);
    }
    if (parts.size() == 1) {
        return parts.front();
    }
    return Value::composite(std::move(parts));
}

[[nodiscard]] bool keyHasNullPart(const Value &key) {
    if (key.isNull()) {
        return true;
    }
    try {
        for (const auto &part : key.compositeParts()) {
            if (part.isNull()) {
                return true;
            }
        }
        return false;
    } catch (const std::runtime_error &) {
        return false;
    }
}

[[nodiscard]] bool rowMatchesKey(const Table &table, const Row &row,
                                 std::span<const std::string> columns, const Value &key) {
    const auto actual = keyFromRow(table, row, columns);
    return actual && *actual == key;
}

bool parentKeyVisible(Table &parent, std::span<const std::string> parentColumns, const Value &value,
                      const ReadSnapshot &snapshot, TransactionManager &transactions) {
    auto hits = parent.indexedLookup(parentColumns, value);
    if (!hits || hits->empty()) {
        for (const auto &[rowId, row] : parent.visibleEntries(snapshot, transactions)) {
            (void)rowId;
            if (rowMatchesKey(parent, row, parentColumns, value)) {
                return true;
            }
        }
        return false;
    }
    const auto visible = parent.visibleEntriesById(*hits, snapshot, transactions);
    return !visible.empty();
}

[[nodiscard]] std::vector<std::pair<RowId, Row>>
visibleChildrenForKey(Table &child, std::span<const std::string> childColumns, const Value &key,
                      const ReadSnapshot &snapshot, TransactionManager &transactions) {
    if (auto hits = child.indexedLookup(childColumns, key)) {
        return child.visibleEntriesById(*hits, snapshot, transactions);
    }
    std::vector<std::pair<RowId, Row>> matches;
    for (const auto &[rowId, row] : child.visibleEntries(snapshot, transactions)) {
        if (rowMatchesKey(child, row, childColumns, key) && !keyHasNullPart(key)) {
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
                                   std::span<const Column> creatingSchema,
                                   std::span<const UniqueConstraint> creatingUniques) {
    for (const auto &fk : foreignKeys) {
        if (fk.childColumns.empty() || fk.parentColumns.empty()) {
            throw std::invalid_argument("FOREIGN KEY requires at least one column");
        }
        if (fk.childColumns.size() != fk.parentColumns.size()) {
            throw std::invalid_argument("FOREIGN KEY child/parent column count mismatch");
        }

        std::span<const Column> parentSchema;
        std::vector<UniqueConstraint> parentUniques;
        if (fk.parentTable == childTableName && !creatingSchema.empty()) {
            parentSchema = creatingSchema;
            parentUniques = uniqueConstraintsFromSchema(creatingSchema, creatingUniques);
        } else {
            auto parent = database.table(fk.parentTable);
            if (!parent) {
                throw std::invalid_argument("FOREIGN KEY parent table not found: " +
                                            fk.parentTable);
            }
            parentSchema = parent->schema();
            parentUniques = parent->allUniqueConstraints();
        }

        for (std::size_t i = 0; i < fk.childColumns.size(); ++i) {
            const auto *childCol = findColumn(childSchema, fk.childColumns[i]);
            if (childCol == nullptr) {
                throw std::invalid_argument("FOREIGN KEY child column not found: " +
                                            fk.childColumns[i]);
            }
            const auto *parentCol = findColumn(parentSchema, fk.parentColumns[i]);
            if (parentCol == nullptr) {
                throw std::invalid_argument("FOREIGN KEY parent column not found: " +
                                            fk.parentColumns[i]);
            }
            if (parentCol->type != childCol->type) {
                throw std::invalid_argument(
                    "FOREIGN KEY column type mismatch: " + fk.childColumns[i] + " vs " +
                    fk.parentTable + "." + fk.parentColumns[i]);
            }
        }

        if (!parentHasExactUniqueKey(parentUniques, fk.parentColumns)) {
            throw std::invalid_argument(
                "FOREIGN KEY parent columns must be PRIMARY KEY or UNIQUE: " + fk.parentTable +
                "(" + foreignKeyColumnsLabel(fk.parentColumns) + ")");
        }
        if (!isSupportedAction(fk.onDelete) || !isSupportedAction(fk.onUpdate)) {
            throw std::invalid_argument(
                "FOREIGN KEY only supports ON DELETE/UPDATE NO ACTION, CASCADE, or SET NULL");
        }
        if (fk.onDelete == ForeignKeyAction::SetNull || fk.onUpdate == ForeignKeyAction::SetNull) {
            for (const auto &childName : fk.childColumns) {
                const auto *childCol = findColumn(childSchema, childName);
                if (childCol == nullptr || !childCol->nullable) {
                    throw std::invalid_argument(
                        "FOREIGN KEY SET NULL requires nullable child column: " + childName);
                }
            }
        }
    }
}

void assertForeignKeysOnChildRow(Database &database, const Table &child, const Row &row,
                                 const ReadSnapshot &snapshot, TransactionManager &transactions) {
    for (const auto &fk : child.foreignKeys()) {
        const auto key = keyFromRow(child, row, fk.childColumns);
        if (!key) {
            throw std::invalid_argument("FOREIGN KEY child column not found: " +
                                        foreignKeyColumnsLabel(fk.childColumns));
        }
        // MATCH SIMPLE: any NULL part skips the parent existence check.
        if (keyHasNullPart(*key)) {
            continue;
        }
        auto parent = database.table(fk.parentTable);
        if (!parent) {
            throw std::invalid_argument("FOREIGN KEY parent table not found: " + fk.parentTable);
        }
        if (!parentKeyVisible(*parent, fk.parentColumns, *key, snapshot, transactions)) {
            throw std::invalid_argument("FOREIGN KEY constraint violation on column " +
                                        foreignKeyColumnsLabel(fk.childColumns));
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
            if (updatingColumn) {
                const auto &schema = parent.schema();
                if (*updatingColumn >= schema.size()) {
                    continue;
                }
                const std::string &updatingName = schema[*updatingColumn].name;
                const bool touchesKey = std::ranges::any_of(
                    fk.parentColumns, [&](const std::string &column) { return column == updatingName; });
                if (!touchesKey) {
                    continue;
                }
            }

            const auto oldKey = keyFromRow(parent, parentRow, fk.parentColumns);
            if (!oldKey || keyHasNullPart(*oldKey)) {
                continue;
            }
            if (updatingColumn && newValue != nullptr) {
                auto updatedParent = parentRow;
                updatedParent[*updatingColumn] = *newValue;
                const auto newKey = keyFromRow(parent, updatedParent, fk.parentColumns);
                if (newKey && *newKey == *oldKey) {
                    continue;
                }
            }
            for (auto &[rowId, row] : visibleChildrenForKey(*other, fk.childColumns, *oldKey,
                                                           snapshot, transactions)) {
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

bool columnIsForeignKeyParent(const Database &database, std::string_view tableName,
                              std::string_view columnName) {
    for (const auto &other : database.tables()) {
        for (const auto &fk : other->foreignKeys()) {
            if (fk.parentTable != tableName) {
                continue;
            }
            for (const auto &parentColumn : fk.parentColumns) {
                if (parentColumn == columnName) {
                    return true;
                }
            }
        }
    }
    return false;
}

bool columnIsForeignKeyChild(const Table &table, std::string_view columnName) {
    for (const auto &fk : table.foreignKeys()) {
        for (const auto &childColumn : fk.childColumns) {
            if (childColumn == columnName) {
                return true;
            }
        }
    }
    return false;
}

std::string foreignKeyLiteral(const ForeignKeyConstraint &fk) {
    std::string sql = "FOREIGN KEY (" + foreignKeyColumnsLabel(fk.childColumns) + ") REFERENCES " +
                      fk.parentTable + "(" + foreignKeyColumnsLabel(fk.parentColumns) + ")";
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
