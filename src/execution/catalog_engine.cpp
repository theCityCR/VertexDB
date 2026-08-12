#include "VertexDB/execution/catalog_engine.hpp"

#include "VertexDB/execution/select_engine.hpp"
#include "VertexDB/execution/select_helpers.hpp"
#include "VertexDB/execution/sql_literal.hpp"
#include "VertexDB/indexing/index_manager.hpp"
#include "VertexDB/transaction/undo_log.hpp"

#include <optional>
#include <utility>

namespace VertexDB {

CatalogEngine::CatalogEngine(ExecutionContext &ctx, RecoveryService &recovery,
                             StorageManager &storage, WriteAheadLog &wal) noexcept
    : ctx_(ctx), recovery_(recovery), storage_(storage), wal_(wal) {}

QueryResult CatalogEngine::executeCreateDatabase(const CreateDatabase &command) {
    if (const auto rejected = ctx_.session.rejectIfTransactionActive("CREATE DATABASE");
        !rejected.success) {
        return rejected;
    }
    recovery_.appendWal(WalOperation::CreateDatabase, command.name);
    ctx_.database = std::make_shared<Database>(command.name);
    return messageResult(true, "created database " + command.name);
}

QueryResult CatalogEngine::executeCreateTable(const CreateTable &command) {
    if (const auto rejected = ctx_.session.rejectIfTransactionActive("CREATE TABLE");
        !rejected.success) {
        return rejected;
    }
    if (!ctx_.database) {
        return messageResult(false, "no active database");
    }
    const bool created = ctx_.database->createTable(command.name, command.columns);
    if (created) {
        recovery_.appendWal(WalOperation::CreateTable, createTableSql(command));
    }
    return messageResult(created,
                         created ? "created table " + command.name : "table already exists");
}

QueryResult CatalogEngine::executeDropTable(const DropTable &command) {
    if (const auto rejected = ctx_.session.rejectIfTransactionActive("DROP TABLE");
        !rejected.success) {
        return rejected;
    }
    if (!ctx_.database) {
        return messageResult(false, "no active database");
    }
    const bool dropped = ctx_.database->dropTable(command.name);
    if (dropped) {
        recovery_.appendWal(WalOperation::DropTable, "DROP TABLE " + command.name + ";");
    }
    return messageResult(dropped, dropped ? "dropped table " + command.name : "unknown table");
}

QueryResult CatalogEngine::executeRenameTable(const RenameTable &command) {
    if (const auto rejected = ctx_.session.rejectIfTransactionActive("RENAME TABLE");
        !rejected.success) {
        return rejected;
    }
    if (!ctx_.database) {
        return messageResult(false, "no active database");
    }
    const bool renamed = ctx_.database->renameTable(command.oldName, command.newName);
    if (renamed) {
        recovery_.appendWal(WalOperation::RenameTable,
                            "RENAME TABLE " + command.oldName + " TO " + command.newName + ";");
    }
    return messageResult(renamed, renamed ? "renamed table " + command.oldName : "rename failed");
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
                           : table->createIndex(command.name, command.column);
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
    if (!table->dropIndex(command.name)) {
        return messageResult(false, "index drop failed");
    }
    if (ctx_.session.transactionActive()) {
        UndoRecord undo;
        undo.tableName = command.table;
        undo.kind = UndoKind::DropIndex;
        undo.indexName = definition->name;
        undo.indexColumn = definition->column;
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
    if (const auto rejected = ctx_.session.rejectIfTransactionActive("SAVE DATABASE");
        !rejected.success) {
        return rejected;
    }
    if (!ctx_.database) {
        return messageResult(false, "no active database");
    }
    recovery_.appendWal(WalOperation::SaveDatabase, ctx_.database->name());
    storage_.saveDatabase(*ctx_.database);
    wal_.reset();
    return messageResult(true, "saved database " + ctx_.database->name());
}

QueryResult CatalogEngine::executeLoadDatabase(const LoadDatabase &command) {
    if (const auto rejected = ctx_.session.rejectIfTransactionActive("LOAD DATABASE");
        !rejected.success) {
        return rejected;
    }
    if (command.name) {
        ctx_.database = storage_.loadDatabase(*command.name);
    } else if (ctx_.database) {
        ctx_.database = storage_.loadDatabase(ctx_.database->name());
    } else {
        ctx_.database = storage_.loadFirstDatabase();
    }
    ctx_.session.reset();
    return messageResult(true, "loaded database " + ctx_.database->name());
}

} // namespace VertexDB
