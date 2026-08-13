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

} // namespace

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
