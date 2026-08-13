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
namespace {

void applyChildFkUpdates(ExecutionContext &ctx, RecoveryService &recovery,
                         std::span<const ForeignKeyChildHit> refs, ForeignKeyAction wanted,
                         const Value &replacement) {
    auto &txns = ctx.session.transactionManager();
    const auto snapshot = ctx.readSnapshot();
    const auto writerId = ctx.session.writeTransactionId();
    for (const auto &hit : refs) {
        if (hit.fk.onUpdate != wanted) {
            continue;
        }
        const auto childIndex = hit.table->columnIndex(hit.fk.childColumn);
        if (!childIndex) {
            throw std::invalid_argument("FOREIGN KEY child column not found: " + hit.fk.childColumn);
        }
        auto updated = hit.row;
        updated[*childIndex] = replacement;
        hit.table->validateRow(updated);
        hit.table->assertUniqueRow(updated, hit.rowId);
        assertForeignKeysOnChildRow(*ctx.database, *hit.table, updated, snapshot, txns);

        const Row beforeImage = hit.row;
        hit.table->clearDirtyTracking();
        if (hit.table->update(hit.rowId, *childIndex, replacement, writerId, &txns)) {
            if (ctx.session.transactionActive()) {
                ctx.session.pushUndo(
                    UndoRecord{hit.tableName, UndoKind::Update, hit.rowId, std::move(beforeImage)});
            }
            recovery.appendPageImageRedo(*hit.table, hit.tableName);
        }
    }
}

} // namespace

DmlEngine::DmlEngine(ExecutionContext &ctx, RecoveryService &recovery) noexcept
    : ctx_(ctx), recovery_(recovery) {}

void DmlEngine::appendPageImageRedo(Table &table, std::string tableName) {
    recovery_.appendPageImageRedo(table, std::move(tableName));
}

QueryResult DmlEngine::executeInsert(const Insert &command) {
    auto table = ctx_.select->requireTable(command.table);
    const auto snapshot = ctx_.readSnapshot();
    auto &txns = ctx_.session.transactionManager();
    for (std::size_t i = 0; i < command.rows.size(); ++i) {
        const auto &row = command.rows[i];
        table->validateRow(row);
        table->assertUniqueRow(row);
        assertForeignKeysOnChildRow(*ctx_.database, *table, row, snapshot, txns);
        for (std::size_t j = 0; j < i; ++j) {
            if (table->rowsConflictOnUnique(row, command.rows[j])) {
                throw std::invalid_argument("unique constraint violation on batch insert");
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

void DmlEngine::eraseRowWithReferentialActions(Table &table, std::string tableName, RowId rowId,
                                               Row row, std::size_t depth,
                                               std::unordered_set<std::string> &visiting,
                                               std::unordered_set<std::string> &deleted) {
    if (depth > kMaxReferentialActionDepth) {
        throw std::invalid_argument("FOREIGN KEY CASCADE depth exceeded");
    }
    const std::string visitKey = tableName + "#" + std::to_string(rowId);
    if (deleted.contains(visitKey)) {
        return;
    }
    if (!visiting.insert(visitKey).second) {
        // Self-referential cycle already being deleted in this cascade chain.
        return;
    }

    auto &txns = ctx_.session.transactionManager();
    const auto snapshot = ctx_.readSnapshot();
    const auto writerId = ctx_.session.writeTransactionId();
    const auto refs = collectReferencingChildren(*ctx_.database, table, row, snapshot, txns);
    assertNoActionParentKeyNotReferenced(refs, /*forUpdate=*/false);

    for (const auto &hit : refs) {
        if (hit.fk.onDelete != ForeignKeyAction::Cascade) {
            continue;
        }
        eraseRowWithReferentialActions(*hit.table, hit.tableName, hit.rowId, hit.row, depth + 1,
                                       visiting, deleted);
    }

    for (const auto &hit : refs) {
        if (hit.fk.onDelete != ForeignKeyAction::SetNull) {
            continue;
        }
        const std::string childKey = hit.tableName + "#" + std::to_string(hit.rowId);
        if (deleted.contains(childKey)) {
            continue;
        }
        const auto childIndex = hit.table->columnIndex(hit.fk.childColumn);
        if (!childIndex) {
            throw std::invalid_argument("FOREIGN KEY child column not found: " + hit.fk.childColumn);
        }
        auto updated = hit.row;
        updated[*childIndex] = Value{};
        hit.table->validateRow(updated);
        hit.table->assertUniqueRow(updated, hit.rowId);
        assertForeignKeysOnChildRow(*ctx_.database, *hit.table, updated, snapshot, txns);

        const Row beforeImage = hit.row;
        hit.table->clearDirtyTracking();
        if (hit.table->update(hit.rowId, *childIndex, Value{}, writerId, &txns)) {
            if (ctx_.session.transactionActive()) {
                ctx_.session.pushUndo(
                    UndoRecord{hit.tableName, UndoKind::Update, hit.rowId, std::move(beforeImage)});
            }
            appendPageImageRedo(*hit.table, hit.tableName);
        }
    }

    if (!deleted.contains(visitKey)) {
        const Row beforeImage = row;
        table.clearDirtyTracking();
        if (table.erase(rowId, writerId, &txns)) {
            deleted.insert(visitKey);
            if (ctx_.session.transactionActive()) {
                ctx_.session.pushUndo(UndoRecord{std::move(tableName), UndoKind::Delete, rowId,
                                                 std::move(beforeImage)});
            }
            appendPageImageRedo(table, table.name());
        }
    }
    visiting.erase(visitKey);
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

        // Referential actions on children that reference this row's old key.
        // SET NULL first (does not need the new parent key). CASCADE after the parent
        // update so the new key is SI-visible when child FK images are validated.
        const auto refs = collectReferencingChildren(*ctx_.database, *table, row, snapshot, txns,
                                                     *target, &command.value);
        assertNoActionParentKeyNotReferenced(refs, /*forUpdate=*/true);
        applyChildFkUpdates(ctx_, recovery_, refs, ForeignKeyAction::SetNull, Value{});

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

        applyChildFkUpdates(ctx_, recovery_, refs, ForeignKeyAction::Cascade, command.value);
    }
    return messageResult(true, "updated " + std::to_string(count) + " row(s)");
}

QueryResult DmlEngine::executeDelete(const Delete &command) {
    auto table = ctx_.select->requireTable(command.table);
    const Select scan = mutationScanSelect(command.table, command.where);
    const QueryPlan plan = ctx_.planner.planSelect(scan, *table);
    const auto targets = ctx_.select->collectVisibleEntries(scan, *table, plan);

    std::size_t count = 0;
    std::unordered_set<std::string> visiting;
    std::unordered_set<std::string> deleted;
    for (const auto &[rowId, row] : targets) {
        const std::string visitKey = command.table + "#" + std::to_string(rowId);
        if (deleted.contains(visitKey)) {
            // Already removed as part of an earlier CASCADE in this statement.
            continue;
        }
        eraseRowWithReferentialActions(*table, command.table, rowId, row, 1, visiting, deleted);
        if (deleted.contains(visitKey)) {
            ++count;
        }
    }
    return messageResult(true, "deleted " + std::to_string(count) + " row(s)");
}

} // namespace VertexDB
