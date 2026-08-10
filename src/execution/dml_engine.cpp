#include "VertexDB/execution/dml_engine.hpp"

#include "VertexDB/execution/select_engine.hpp"
#include "VertexDB/execution/select_helpers.hpp"
#include "VertexDB/transaction/undo_log.hpp"

#include <stdexcept>
#include <utility>

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

    std::size_t count = 0;
    const auto snapshot = ctx_.readSnapshot();
    const auto writerId = ctx_.session.writeTransactionId();
    for (const auto &[rowId, row] :
         table->visibleEntries(snapshot, ctx_.session.transactionManager())) {
        if (command.where && !ctx_.select->matches(row, *table, *command.where)) {
            continue;
        }
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
    std::size_t count = 0;
    const auto snapshot = ctx_.readSnapshot();
    const auto writerId = ctx_.session.writeTransactionId();
    for (const auto &[rowId, row] :
         table->visibleEntries(snapshot, ctx_.session.transactionManager())) {
        if (command.where && !ctx_.select->matches(row, *table, *command.where)) {
            continue;
        }
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
