#include "VertexDB/execution/query_executor.hpp"

#include "VertexDB/common/string_utils.hpp"
#include "VertexDB/execution/prepared_bind.hpp"
#include "VertexDB/execution/select_helpers.hpp"
#include "VertexDB/execution/sql_literal.hpp"
#include "VertexDB/parser/parser.hpp"

#include <stdexcept>
#include <utility>

namespace VertexDB {

QueryExecutor::QueryExecutor(std::filesystem::path storageRoot)
    : storageManager_(storageRoot), wal_(storageRoot / "VertexDB.wal") {
    recoverFromStorage();
}

QueryResult QueryExecutor::execute(const Query &query) {
    if (std::holds_alternative<ExecutePrepared>(query)) {
        return executePrepared(std::get<ExecutePrepared>(query));
    }

    const bool readOnly = std::holds_alternative<Select>(query) ||
                          std::holds_alternative<ListTables>(query) ||
                          std::holds_alternative<ExplainQuery>(query);
    if (readOnly) {
        const auto lock = lockManager_.acquireRead();
        return executeUnlocked(query);
    }

    const auto lock = lockManager_.acquireWrite();
    return executeUnlocked(query);
}

QueryResult QueryExecutor::executeUnlocked(const Query &query) {
    return std::visit(
        [this](const auto &command) -> QueryResult {
            using Command = std::decay_t<decltype(command)>;
            if constexpr (std::is_same_v<Command, CreateDatabase>) {
                return executeCreateDatabase(command);
            } else if constexpr (std::is_same_v<Command, CreateTable>) {
                return executeCreateTable(command);
            } else if constexpr (std::is_same_v<Command, DropTable>) {
                return executeDropTable(command);
            } else if constexpr (std::is_same_v<Command, RenameTable>) {
                return executeRenameTable(command);
            } else if constexpr (std::is_same_v<Command, ListTables>) {
                return executeListTables();
            } else if constexpr (std::is_same_v<Command, Insert>) {
                return executeInsert(command);
            } else if constexpr (std::is_same_v<Command, Select>) {
                return executeSelect(command);
            } else if constexpr (std::is_same_v<Command, ExplainQuery>) {
                return executeExplain(command);
            } else if constexpr (std::is_same_v<Command, Update>) {
                return executeUpdate(command);
            } else if constexpr (std::is_same_v<Command, Delete>) {
                return executeDelete(command);
            } else if constexpr (std::is_same_v<Command, CreateIndex>) {
                return executeCreateIndex(command);
            } else if constexpr (std::is_same_v<Command, Analyze>) {
                return executeAnalyze(command);
            } else if constexpr (std::is_same_v<Command, SaveDatabase>) {
                return executeSaveDatabase();
            } else if constexpr (std::is_same_v<Command, LoadDatabase>) {
                return executeLoadDatabase(command);
            } else if constexpr (std::is_same_v<Command, BeginTransaction>) {
                return executeBegin();
            } else if constexpr (std::is_same_v<Command, CommitTransaction>) {
                return executeCommit();
            } else if constexpr (std::is_same_v<Command, RollbackTransaction>) {
                return executeRollback();
            } else if constexpr (std::is_same_v<Command, PrepareStatement>) {
                return executePrepare(command);
            } else if constexpr (std::is_same_v<Command, ExecutePrepared>) {
                throw std::runtime_error("prepared execution must not be dispatched while locked");
            } else if constexpr (std::is_same_v<Command, Exit>) {
                return messageResult(true, "exit");
            }
        },
        query);
}

std::shared_ptr<Database> QueryExecutor::currentDatabase() const noexcept { return database_; }

QueryResult QueryExecutor::executeCreateDatabase(const CreateDatabase &command) {
    if (const auto rejected = rejectIfTransactionActive("CREATE DATABASE"); !rejected.success) {
        return rejected;
    }
    appendWal(WalOperation::CreateDatabase, command.name);
    database_ = std::make_shared<Database>(command.name);
    return messageResult(true, "created database " + command.name);
}

QueryResult QueryExecutor::executeCreateTable(const CreateTable &command) {
    if (const auto rejected = rejectIfTransactionActive("CREATE TABLE"); !rejected.success) {
        return rejected;
    }
    if (!database_) {
        return messageResult(false, "no active database");
    }
    const bool created = database_->createTable(command.name, command.columns);
    if (created) {
        appendWal(WalOperation::CreateTable, createTableSql(command));
    }
    return messageResult(created,
                         created ? "created table " + command.name : "table already exists");
}

QueryResult QueryExecutor::executeDropTable(const DropTable &command) {
    if (const auto rejected = rejectIfTransactionActive("DROP TABLE"); !rejected.success) {
        return rejected;
    }
    if (!database_) {
        return messageResult(false, "no active database");
    }
    const bool dropped = database_->dropTable(command.name);
    if (dropped) {
        appendWal(WalOperation::DropTable, "DROP TABLE " + command.name + ";");
    }
    return messageResult(dropped, dropped ? "dropped table " + command.name : "unknown table");
}

QueryResult QueryExecutor::executeRenameTable(const RenameTable &command) {
    if (const auto rejected = rejectIfTransactionActive("RENAME TABLE"); !rejected.success) {
        return rejected;
    }
    if (!database_) {
        return messageResult(false, "no active database");
    }
    const bool renamed = database_->renameTable(command.oldName, command.newName);
    if (renamed) {
        appendWal(WalOperation::RenameTable,
                  "RENAME TABLE " + command.oldName + " TO " + command.newName + ";");
    }
    return messageResult(renamed, renamed ? "renamed table " + command.oldName : "rename failed");
}

QueryResult QueryExecutor::executeListTables() {
    if (!database_) {
        return messageResult(false, "no active database");
    }
    QueryResult result;
    result.message = "listed tables";
    result.columns = {"table"};
    for (const auto &name : database_->listTables()) {
        result.rows.push_back({Value{name}});
    }
    return result;
}

QueryResult QueryExecutor::executeInsert(const Insert &command) {
    auto table = requireTable(command.table);
    for (const auto &row : command.rows) {
        table->validateRow(row);
    }
    const auto writerId = writeTransactionId();
    for (const auto &row : command.rows) {
        table->clearDirtyTracking();
        const RowId rowId = table->insert(row, writerId);
        if (transactionActive()) {
            undoLog_.push(UndoRecord{command.table, UndoKind::Insert, rowId, std::nullopt});
        }
        appendPageImageRedo(*table, command.table);
    }
    return messageResult(true, "inserted " + std::to_string(command.rows.size()) + " row(s)");
}

QueryResult QueryExecutor::executeUpdate(const Update &command) {
    auto table = requireTable(command.table);
    const auto target = table->columnIndex(command.column);
    if (!target) {
        throw std::runtime_error("unknown update column");
    }

    std::size_t count = 0;
    const auto snapshot = readSnapshot();
    const auto writerId = writeTransactionId();
    for (const auto &[rowId, row] : table->visibleEntries(snapshot, transactionManager_)) {
        if (command.where && !matches(row, *table, *command.where)) {
            continue;
        }
        const Row beforeImage = row;
        table->clearDirtyTracking();
        if (table->update(rowId, *target, command.value, writerId)) {
            if (transactionActive()) {
                undoLog_.push(
                    UndoRecord{command.table, UndoKind::Update, rowId, std::move(beforeImage)});
            }
            appendPageImageRedo(*table, command.table);
            ++count;
        }
    }
    return messageResult(true, "updated " + std::to_string(count) + " row(s)");
}

QueryResult QueryExecutor::executeDelete(const Delete &command) {
    auto table = requireTable(command.table);
    std::size_t count = 0;
    const auto snapshot = readSnapshot();
    const auto writerId = writeTransactionId();
    for (const auto &[rowId, row] : table->visibleEntries(snapshot, transactionManager_)) {
        if (command.where && !matches(row, *table, *command.where)) {
            continue;
        }
        const Row beforeImage = row;
        table->clearDirtyTracking();
        if (table->erase(rowId, writerId)) {
            if (transactionActive()) {
                undoLog_.push(
                    UndoRecord{command.table, UndoKind::Delete, rowId, std::move(beforeImage)});
            }
            appendPageImageRedo(*table, command.table);
            ++count;
        }
    }
    return messageResult(true, "deleted " + std::to_string(count) + " row(s)");
}

QueryResult QueryExecutor::executeCreateIndex(const CreateIndex &command) {
    if (const auto rejected = rejectIfTransactionActive("CREATE INDEX"); !rejected.success) {
        return rejected;
    }
    auto table = requireTable(command.table);
    const bool created =
        command.expression ? table->createIndex(command.name, *command.expression)
                           : table->createIndex(command.name, command.column);
    if (created) {
        appendWal(WalOperation::CreateIndex, createIndexSql(command));
    }
    return messageResult(created,
                         created ? "created index " + command.name : "index creation failed");
}

QueryResult QueryExecutor::executeAnalyze(const Analyze &command) {
    if (!database_) {
        return messageResult(false, "no active database");
    }
    if (command.table) {
        auto table = requireTable(*command.table);
        table->analyze();
        return messageResult(true, "analyzed table " + *command.table);
    }
    const auto tables = database_->tables();
    for (const auto &table : tables) {
        table->analyze();
    }
    return messageResult(true, "analyzed " + std::to_string(tables.size()) + " table(s)");
}

QueryResult QueryExecutor::executeSaveDatabase() {
    if (const auto rejected = rejectIfTransactionActive("SAVE DATABASE"); !rejected.success) {
        return rejected;
    }
    if (!database_) {
        return messageResult(false, "no active database");
    }
    appendWal(WalOperation::SaveDatabase, database_->name());
    storageManager_.saveDatabase(*database_);
    wal_.reset();
    return messageResult(true, "saved database " + database_->name());
}

QueryResult QueryExecutor::executeLoadDatabase(const LoadDatabase &command) {
    if (const auto rejected = rejectIfTransactionActive("LOAD DATABASE"); !rejected.success) {
        return rejected;
    }
    if (command.name) {
        database_ = storageManager_.loadDatabase(*command.name);
    } else if (database_) {
        database_ = storageManager_.loadDatabase(database_->name());
    } else {
        database_ = storageManager_.loadFirstDatabase();
    }
    undoLog_.clear();
    clearPendingWal();
    activeTransaction_.reset();
    activeSnapshot_.reset();
    return messageResult(true, "loaded database " + database_->name());
}

QueryResult QueryExecutor::executeBegin() {
    if (!database_) {
        return messageResult(false, "no active database");
    }
    if (transactionActive()) {
        return messageResult(false, "transaction already active");
    }
    activeTransaction_ = transactionManager_.begin().id;
    activeSnapshot_ = transactionManager_.currentSnapshot(*activeTransaction_);
    undoLog_.clear();
    clearPendingWal();
    return messageResult(true, "began transaction");
}

QueryResult QueryExecutor::executeCommit() {
    if (!transactionActive()) {
        return messageResult(false, "no active transaction");
    }
    flushPendingWal();
    transactionManager_.commit(*activeTransaction_);
    activeTransaction_.reset();
    activeSnapshot_.reset();
    undoLog_.clear();
    return messageResult(true, "committed transaction");
}

QueryResult QueryExecutor::executeRollback() {
    if (!transactionActive()) {
        return messageResult(false, "no active transaction");
    }
    while (auto record = undoLog_.pop()) {
        applyUndoRecord(*record);
    }
    transactionManager_.rollback(*activeTransaction_);
    activeTransaction_.reset();
    activeSnapshot_.reset();
    undoLog_.clear();
    clearPendingWal();
    return messageResult(true, "rolled back transaction");
}

QueryResult QueryExecutor::executePrepare(const PrepareStatement &command) {
    // Parse once into a typed AST with Parameter slots; EXECUTE binds without reparsing.
    auto ast = Parser{}.parse(command.sql);
    preparedStatements_[command.name] = std::move(ast);
    return messageResult(true, "prepared statement " + command.name);
}

QueryResult QueryExecutor::executePrepared(const ExecutePrepared &command) {
    Query stored;
    {
        const auto lock = lockManager_.acquireRead();
        auto prepared = preparedStatements_.find(command.name);
        if (prepared == preparedStatements_.end()) {
            throw std::runtime_error("unknown prepared statement");
        }
        stored = prepared->second;
    }
    const Query bound = bindQueryParameters(stored, command.parameters);
    return execute(bound);
}

std::optional<Query> QueryExecutor::preparedAst(std::string_view name) const {
    const auto lock = lockManager_.acquireRead();
    for (const auto &[storedName, query] : preparedStatements_) {
        if (equalsIgnoreCase(storedName, name)) {
            return query;
        }
    }
    return std::nullopt;
}

ReadSnapshot QueryExecutor::readSnapshot() const {
    if (activeSnapshot_) {
        return *activeSnapshot_;
    }
    return transactionManager_.currentSnapshot();
}

TransactionId QueryExecutor::writeTransactionId() {
    if (activeTransaction_) {
        return *activeTransaction_;
    }
    return transactionManager_.beginCommitted();
}

bool QueryExecutor::transactionActive() const noexcept { return activeTransaction_.has_value(); }

QueryResult QueryExecutor::rejectIfTransactionActive(std::string_view action) const {
    if (!transactionActive()) {
        return messageResult(true, {});
    }
    return messageResult(false, std::string(action) + " is not allowed while a transaction is active");
}

} // namespace VertexDB
