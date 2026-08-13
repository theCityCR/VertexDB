#include "VertexDB/execution/dml_engine.hpp"

#include "VertexDB/execution/foreign_key_eval.hpp"
#include "VertexDB/execution/select_engine.hpp"
#include "VertexDB/execution/select_helpers.hpp"
#include "VertexDB/planner/query_planner.hpp"
#include "VertexDB/transaction/undo_log.hpp"

#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace VertexDB {

DmlEngine::DmlEngine(ExecutionContext &ctx, RecoveryService &recovery) noexcept
    : ctx_(ctx), recovery_(recovery) {}

void DmlEngine::appendPageImageRedo(Table &table, std::string tableName) {
    recovery_.appendPageImageRedo(table, std::move(tableName));
}

QueryResult DmlEngine::executeInsert(const Insert &command) {
    auto table = ctx_.select->requireTable(command.table);
    const auto schema = table->schema();
    const auto snapshot = ctx_.readSnapshot();
    auto &txns = ctx_.session.transactionManager();
    for (std::size_t i = 0; i < command.rows.size(); ++i) {
        const auto &row = command.rows[i];
        table->validateRow(row);
        table->assertUniqueRow(row);
        assertForeignKeysOnChildRow(*ctx_.database, *table, row, snapshot, txns);
        for (std::size_t j = 0; j < i; ++j) {
            const auto &prior = command.rows[j];
            for (std::size_t col = 0; col < schema.size(); ++col) {
                if (!schema[col].unique && !schema[col].primaryKey) {
                    continue;
                }
                if (row[col].isNull() || prior[col].isNull()) {
                    continue;
                }
                if (row[col] == prior[col]) {
                    throw std::invalid_argument("unique constraint violation on column " +
                                                schema[col].name);
                }
            }
        }
    }
    const auto writerId = ctx_.session.writeTransactionId();
    for (const auto &row : command.rows) {
        table->clearDirtyTracking();
        const RowId rowId = table->insert(row, writerId, &txns);
        if (ctx_.session.transactionActive()) {
            ctx_.session.pushUndo(
                UndoRecord{command.table, UndoKind::Insert, rowId, std::nullopt});
        }
        appendPageImageRedo(*table, command.table);
    }
    return messageResult(true, "inserted " + std::to_string(command.rows.size()) + " row(s)");
}

QueryResult DmlEngine::executeUpdate(const Update &command) {
    auto table = ctx_.select->requireTable(command.table);
    const auto target = table->columnIndex(command.column);
    if (!target) {
        throw std::runtime_error("unknown update column");
    }

    const Select scan = mutationScanSelect(command.table, command.where);
    const QueryPlan plan = ctx_.planner.planSelect(scan, *table);
    // Collect all targets before mutating so index rebuilds mid-statement cannot skip hits.
    const auto targets = ctx_.select->collectVisibleEntries(scan, *table, plan);

    std::size_t count = 0;
    const auto writerId = ctx_.session.writeTransactionId();
    auto &txns = ctx_.session.transactionManager();
    const auto snapshot = ctx_.readSnapshot();
    for (const auto &[rowId, row] : targets) {
        auto updated = row;
        updated[*target] = command.value;
        table->validateRow(updated);
        table->assertUniqueRow(updated, rowId);
        assertForeignKeysOnChildRow(*ctx_.database, *table, updated, snapshot, txns);
        assertParentKeyNotReferenced(*ctx_.database, *table, row, snapshot, txns, *target,
                                     &command.value);

        const Row beforeImage = row;
        table->clearDirtyTracking();
        if (table->update(rowId, *target, command.value, writerId, &txns)) {
            if (ctx_.session.transactionActive()) {
                ctx_.session.pushUndo(
                    UndoRecord{command.table, UndoKind::Update, rowId, std::move(beforeImage)});
            }
            appendPageImageRedo(*table, command.table);
            ++count;
        }
    }
    return messageResult(true, "updated " + std::to_string(count) + " row(s)");
}

QueryResult DmlEngine::executeDelete(const Delete &command) {
    auto table = ctx_.select->requireTable(command.table);
    const Select scan = mutationScanSelect(command.table, command.where);
    const QueryPlan plan = ctx_.planner.planSelect(scan, *table);
    const auto targets = ctx_.select->collectVisibleEntries(scan, *table, plan);

    std::size_t count = 0;
    const auto writerId = ctx_.session.writeTransactionId();
    auto &txns = ctx_.session.transactionManager();
    const auto snapshot = ctx_.readSnapshot();
    for (const auto &[rowId, row] : targets) {
        assertParentKeyNotReferenced(*ctx_.database, *table, row, snapshot, txns);

        const Row beforeImage = row;
        table->clearDirtyTracking();
        if (table->erase(rowId, writerId, &txns)) {
            if (ctx_.session.transactionActive()) {
                ctx_.session.pushUndo(
                    UndoRecord{command.table, UndoKind::Delete, rowId, std::move(beforeImage)});
            }
            appendPageImageRedo(*table, command.table);
            ++count;
        }
    }
    return messageResult(true, "deleted " + std::to_string(count) + " row(s)");
}

} // namespace VertexDB
