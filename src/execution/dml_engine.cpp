#include "VertexDB/execution/dml_engine.hpp"

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
    for (const auto &row : command.rows) {
        table->validateRow(row);
    }
    const auto writerId = ctx_.session.writeTransactionId();
    for (const auto &row : command.rows) {
        table->clearDirtyTracking();
        const RowId rowId = table->insert(row, writerId);
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
    for (const auto &[rowId, row] : targets) {
        const Row beforeImage = row;
        table->clearDirtyTracking();
        if (table->update(rowId, *target, command.value, writerId)) {
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
    for (const auto &[rowId, row] : targets) {
        const Row beforeImage = row;
        table->clearDirtyTracking();
        if (table->erase(rowId, writerId)) {
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
