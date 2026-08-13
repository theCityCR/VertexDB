#include "VertexDB/execution/catalog_engine.hpp"

#include "VertexDB/execution/foreign_key_eval.hpp"
#include "VertexDB/execution/select_engine.hpp"
#include "VertexDB/execution/select_helpers.hpp"
#include "VertexDB/execution/sql_literal.hpp"
#include "VertexDB/indexing/index_manager.hpp"
#include "VertexDB/transaction/undo_log.hpp"

#include <optional>
#include <type_traits>
#include <utility>

namespace VertexDB {

CatalogEngine::CatalogEngine(ExecutionContext &ctx, RecoveryService &recovery,
                             StorageManager &storage, WriteAheadLog &wal) noexcept
    : ctx_(ctx), recovery_(recovery), storage_(storage), wal_(wal) {}

QueryResult CatalogEngine::executeCreateDatabase(const CreateDatabase &command) {
    if (ctx_.session.transactionActive()) {
        UndoRecord undo;
        undo.kind = UndoKind::SwapDatabase;
        undo.previousDatabase = ctx_.database;
        ctx_.session.pushUndo(std::move(undo));
    }
    recovery_.appendWal(WalOperation::CreateDatabase, command.name);
    ctx_.database = std::make_shared<Database>(command.name);
    return messageResult(true, "created database " + command.name);
}

QueryResult CatalogEngine::executeDropDatabase(const DropDatabase &command) {
    if (ctx_.session.transactionActive()) {
        return messageResult(false, "DROP DATABASE is not allowed while a transaction is active");
    }
    if (!ctx_.database) {
        return messageResult(false, "no active database");
    }
    if (ctx_.database->name() != command.name) {
        return messageResult(false, "DROP DATABASE must name the active database");
    }
    recovery_.appendWal(WalOperation::DropDatabase, command.name);
    ctx_.database.reset();
    storage_.deleteDatabase(command.name);
    return messageResult(true, "dropped database " + command.name);
}

QueryResult CatalogEngine::executeCreateTable(const CreateTable &command) {
    if (!ctx_.database) {
        return messageResult(false, "no active database");
    }
    try {
        validateForeignKeyDefinitions(*ctx_.database, command.name, command.columns,
                                      command.foreignKeys, command.columns,
                                      command.uniqueConstraints);
    } catch (const std::invalid_argument &ex) {
        return messageResult(false, ex.what());
    }
    const bool created =
        ctx_.database->createTable(command.name, command.columns, command.checkConstraints,
                                   command.foreignKeys, command.uniqueConstraints);
    if (created) {
        auto table = ctx_.database->table(command.name);
        table->ensureConstraintIndexes();
        if (ctx_.session.transactionActive()) {
            UndoRecord undo;
            undo.tableName = command.name;
            undo.kind = UndoKind::CreateTable;
            ctx_.session.pushUndo(std::move(undo));
        }
        recovery_.appendWal(WalOperation::CreateTable, createTableSql(command));
    }
    return messageResult(created,
                         created ? "created table " + command.name : "table already exists");
}

QueryResult CatalogEngine::executeDropTable(const DropTable &command) {
    if (!ctx_.database) {
        return messageResult(false, "no active database");
    }
    if (tableIsForeignKeyParent(*ctx_.database, command.name)) {
        return messageResult(false, "cannot DROP TABLE: still referenced by FOREIGN KEY");
    }
    auto retained = ctx_.database->detachTable(command.name);
    if (!retained) {
        return messageResult(false, "unknown table");
    }
    if (ctx_.session.transactionActive()) {
        UndoRecord undo;
        undo.tableName = command.name;
        undo.kind = UndoKind::DropTable;
        undo.retainedTable = std::move(retained);
        ctx_.session.pushUndo(std::move(undo));
    }
    recovery_.appendWal(WalOperation::DropTable, "DROP TABLE " + command.name + ";");
    return messageResult(true, "dropped table " + command.name);
}

QueryResult CatalogEngine::executeRenameTable(const RenameTable &command) {
    if (!ctx_.database) {
        return messageResult(false, "no active database");
    }
    if (tableIsForeignKeyParent(*ctx_.database, command.oldName)) {
        return messageResult(false, "cannot RENAME TABLE: still referenced by FOREIGN KEY");
    }
    const bool renamed = ctx_.database->renameTable(command.oldName, command.newName);
    if (renamed) {
        if (ctx_.session.transactionActive()) {
            ctx_.session.rewriteTableName(command.oldName, command.newName);
            UndoRecord undo;
            undo.tableName = command.oldName;
            undo.renameTo = command.newName;
            undo.kind = UndoKind::RenameTable;
            ctx_.session.pushUndo(std::move(undo));
        }
        recovery_.appendWal(WalOperation::RenameTable,
                            "RENAME TABLE " + command.oldName + " TO " + command.newName + ";");
    }
    return messageResult(renamed, renamed ? "renamed table " + command.oldName : "rename failed");
}

QueryResult CatalogEngine::executeAlterTable(const AlterTable &command) {
    if (!ctx_.database) {
        return messageResult(false, "no active database");
    }
    auto table = ctx_.database->table(command.table);
    if (!table) {
        return messageResult(false, "unknown table");
    }
    try {
        return std::visit(
            [&](const auto &action) -> QueryResult {
                using T = std::decay_t<decltype(action)>;
                if constexpr (std::is_same_v<T, AlterAddColumn>) {
                    table->addColumn(action.column, action.defaultValue);
                    if (ctx_.session.transactionActive()) {
                        UndoRecord undo;
                        undo.tableName = command.table;
                        undo.kind = UndoKind::AlterAddColumn;
                        undo.alterColumn = action.column;
                        ctx_.session.pushUndo(std::move(undo));
                    }
                    recovery_.appendWal(WalOperation::AlterTable, alterTableSql(command));
                    return messageResult(true, "added column " + action.column.name);
                } else if constexpr (std::is_same_v<T, AlterDropColumn>) {
                    auto capture =
                        table->dropColumn(action.column, ctx_.database.get(), action.cascade);
                    if (ctx_.session.transactionActive()) {
                        UndoRecord undo;
                        undo.tableName = command.table;
                        undo.kind = UndoKind::AlterDropColumn;
                        undo.alterColumn = capture.column;
                        undo.alterColumnIndex = capture.columnIndex;
                        undo.alterHeapColumnValues = std::move(capture.heapValues);
                        undo.alterVersionColumnValues = std::move(capture.versionValues);
                        undo.alterCascaded = capture.cascaded;
                        undo.alterDroppedUserIndexes = std::move(capture.droppedUserIndexes);
                        undo.alterDroppedChecks = std::move(capture.droppedChecks);
                        undo.alterDroppedUniques = std::move(capture.droppedUniques);
                        undo.alterDroppedChildForeignKeys =
                            std::move(capture.droppedChildForeignKeys);
                        ctx_.session.pushUndo(std::move(undo));
                    }
                    recovery_.appendWal(WalOperation::AlterTable, alterTableSql(command));
                    return messageResult(true, "dropped column " + action.column);
                } else {
                    static_assert(std::is_same_v<T, AlterRenameColumn>);
                    table->renameColumn(action.oldName, action.newName, ctx_.database.get());
                    if (ctx_.session.transactionActive()) {
                        UndoRecord undo;
                        undo.tableName = command.table;
                        undo.kind = UndoKind::AlterRenameColumn;
                        undo.alterColumn = Column{action.oldName, ColumnType::Int, false, false,
                                                  false};
                        undo.renameTo = action.newName;
                        ctx_.session.pushUndo(std::move(undo));
                    }
                    recovery_.appendWal(WalOperation::AlterTable, alterTableSql(command));
                    return messageResult(true, "renamed column " + action.oldName + " to " +
                                                   action.newName);
                }
            },
            command.action);
    } catch (const std::invalid_argument &ex) {
        return messageResult(false, ex.what());
    } catch (const std::runtime_error &ex) {
        return messageResult(false, ex.what());
    }
}

QueryResult CatalogEngine::executeListTables() {
    if (!ctx_.database) {
        return messageResult(false, "no active database");
    }
    QueryResult result;
    result.message = "listed tables";
    result.columns = {"table"};
    for (const auto &name : ctx_.database->listTables()) {
        result.rows.push_back({Value{name}});
    }
    return result;
}

QueryResult CatalogEngine::executeCreateIndex(const CreateIndex &command) {
    auto table = ctx_.select->requireTable(command.table);
    const bool created =
        command.expression ? table->createIndex(command.name, *command.expression)
                           : table->createIndex(command.name, command.columns);
    if (created) {
        if (ctx_.session.transactionActive()) {
            UndoRecord undo;
            undo.tableName = command.table;
            undo.kind = UndoKind::CreateIndex;
            undo.indexName = command.name;
            ctx_.session.pushUndo(std::move(undo));
        }
        recovery_.appendWal(WalOperation::CreateIndex, createIndexSql(command));
    }
    return messageResult(created,
                         created ? "created index " + command.name : "index creation failed");
}

QueryResult CatalogEngine::executeDropIndex(const DropIndex &command) {
    auto table = ctx_.select->requireTable(command.table);
    std::optional<IndexDefinition> definition;
    for (const auto &entry : table->indexDefinitions()) {
        if (entry.name == command.name) {
            definition = entry;
            break;
        }
    }
    if (!definition) {
        return messageResult(false, "unknown index");
    }
    if (definition->name.starts_with("__pk_") || definition->name.starts_with("__uq_")) {
        return messageResult(false, "cannot drop constraint index " + definition->name);
    }
    if (!table->dropIndex(command.name)) {
        return messageResult(false, "index drop failed");
    }
    if (ctx_.session.transactionActive()) {
        UndoRecord undo;
        undo.tableName = command.table;
        undo.kind = UndoKind::DropIndex;
        undo.indexName = definition->name;
        undo.indexColumn = definition->column;
        undo.indexColumns = definition->columns;
        undo.indexExpression = definition->expression;
        ctx_.session.pushUndo(std::move(undo));
    }
    recovery_.appendWal(WalOperation::DropIndex, dropIndexSql(command));
    return messageResult(true, "dropped index " + command.name);
}

QueryResult CatalogEngine::executeAnalyze(const Analyze &command) {
    if (!ctx_.database) {
        return messageResult(false, "no active database");
    }
    if (command.table) {
        auto table = ctx_.select->requireTable(*command.table);
        table->analyze();
        return messageResult(true, "analyzed table " + *command.table);
    }
    const auto tables = ctx_.database->tables();
    for (const auto &table : tables) {
        table->analyze();
    }
    return messageResult(true, "analyzed " + std::to_string(tables.size()) + " table(s)");
}

QueryResult CatalogEngine::executeSaveDatabase() {
    if (!ctx_.database) {
        return messageResult(false, "no active database");
    }
    bool implicitCommit = false;
    if (ctx_.session.transactionActive()) {
        if (!ctx_.session.transactionManager().isSerializable(
                *ctx_.session.activeTransactionId())) {
            while (auto record = ctx_.session.undoLog().pop()) {
                recovery_.applyUndoRecord(*record);
            }
            ctx_.session.clearPendingWal();
            (void)ctx_.session.rollback();
            return messageResult(false, "serialization failure");
        }
        recovery_.flushPendingWal();
        const auto committed = ctx_.session.commit();
        if (!committed.success) {
            return committed;
        }
        implicitCommit = true;
    }
    recovery_.appendWal(WalOperation::SaveDatabase, ctx_.database->name());
    storage_.saveDatabase(*ctx_.database);
    wal_.reset();
    return messageResult(true, (implicitCommit ? "committed and saved database "
                                               : "saved database ") +
                                   ctx_.database->name());
}

QueryResult CatalogEngine::executeLoadDatabase(const LoadDatabase &command) {
    bool implicitRollback = false;
    if (ctx_.session.transactionActive()) {
        while (auto record = ctx_.session.undoLog().pop()) {
            recovery_.applyUndoRecord(*record);
        }
        const auto rolled = ctx_.session.rollback();
        if (!rolled.success) {
            return rolled;
        }
        implicitRollback = true;
    }
    if (command.name) {
        ctx_.database = storage_.loadDatabase(*command.name);
    } else if (ctx_.database) {
        ctx_.database = storage_.loadDatabase(ctx_.database->name());
    } else {
        ctx_.database = storage_.loadFirstDatabase();
    }
    ctx_.session.reset();
    return messageResult(true, (implicitRollback ? "rolled back and loaded database "
                                                 : "loaded database ") +
                                   ctx_.database->name());
}

} // namespace VertexDB
