#include "VertexDB/execution/query_executor.hpp"

#include "VertexDB/execution/predicate_eval.hpp"
#include "VertexDB/execution/select_helpers.hpp"
#include "VertexDB/execution/sql_literal.hpp"
#include "VertexDB/parser/parser.hpp"
#include "VertexDB/planner/query_planner.hpp"

#include <map>
#include <sstream>
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
        appendWal(WalOperation::Insert, insertSql(command.table, row));
        const RowId rowId = table->insert(row, writerId);
        if (transactionActive()) {
            undoLog_.push(UndoRecord{command.table, UndoKind::Insert, rowId, std::nullopt});
        }
    }
    return messageResult(true, "inserted " + std::to_string(command.rows.size()) + " row(s)");
}

QueryResult QueryExecutor::executeSelect(const Select &command) {
    RewriteResult rewrite;
    const Select prepared = prepareSelect(command, rewrite);
    if (prepared.join) {
        return executeJoinSelect(prepared);
    }

    auto table = requireTable(prepared.table);
    const auto plan = planPreparedSelect(prepared, *table, rewrite);

    std::vector<std::string> columns;
    const auto projection = resolveProjection(prepared, *table, columns);
    auto rows = collectRows(prepared, *table, plan);

    if (prepared.orderBy) {
        const auto orderIndex = table->columnIndex(prepared.orderBy->column);
        if (!orderIndex) {
            throw std::runtime_error("unknown ORDER BY column");
        }
        sortRowsByColumn(rows, *orderIndex, prepared.orderBy->ascending);
    }

    return projectWithLimit(std::move(rows), projection, std::move(columns), prepared.limit);
}

QueryResult QueryExecutor::executeExplain(const ExplainQuery &command) {
    RewriteResult rewrite;
    const Select prepared = prepareSelect(command.query, rewrite);
    if (prepared.join) {
        QueryResult result;
        result.success = true;
        result.message = "explain";
        result.columns = {"plan"};
        result.rows.push_back({Value{"hash join (planner bypassed for JOIN)"}});
        for (const auto &note : rewrite.notes) {
            result.rows.push_back({Value{note}});
        }
        return result;
    }

    auto table = requireTable(prepared.table);
    const auto plan = planPreparedSelect(prepared, *table, rewrite);
    QueryResult result;
    result.success = true;
    result.message = "explain";
    result.columns = {"plan"};
    result.rows.push_back({Value{formatPlanExplanation(plan)}});
    return result;
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
        if (table->update(rowId, *target, command.value, writerId)) {
            if (transactionActive()) {
                undoLog_.push(
                    UndoRecord{command.table, UndoKind::Update, rowId, std::move(beforeImage)});
            }
            ++count;
        }
    }
    if (count != 0) {
        appendWal(WalOperation::Update, updateSql(command));
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
        if (table->erase(rowId, writerId)) {
            if (transactionActive()) {
                undoLog_.push(
                    UndoRecord{command.table, UndoKind::Delete, rowId, std::move(beforeImage)});
            }
            ++count;
        }
    }
    if (count != 0) {
        appendWal(WalOperation::Delete, deleteSql(command));
    }
    return messageResult(true, "deleted " + std::to_string(count) + " row(s)");
}

QueryResult QueryExecutor::executeCreateIndex(const CreateIndex &command) {
    if (const auto rejected = rejectIfTransactionActive("CREATE INDEX"); !rejected.success) {
        return rejected;
    }
    auto table = requireTable(command.table);
    const bool created = table->createIndex(command.name, command.column);
    if (created) {
        appendWal(WalOperation::CreateIndex, createIndexSql(command));
    }
    return messageResult(created,
                         created ? "created index " + command.name : "index creation failed");
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
    preparedStatements_[command.name] = command.sql;
    return messageResult(true, "prepared statement " + command.name);
}

QueryResult QueryExecutor::executePrepared(const ExecutePrepared &command) {
    const auto sql = bindPreparedSql(command);
    return execute(Parser{}.parse(sql));
}

std::vector<std::size_t> QueryExecutor::resolveProjection(const Select &command, const Table &table,
                                                          std::vector<std::string> &columns) const {
    std::vector<std::size_t> projection;
    if (command.columns.size() == 1 && command.columns.front() == "*") {
        for (std::size_t i = 0; i < table.schema().size(); ++i) {
            projection.push_back(i);
            columns.push_back(table.schema()[i].name);
        }
        return projection;
    }

    for (const auto &column : command.columns) {
        auto index = table.columnIndex(column);
        if (!index) {
            throw std::runtime_error("unknown selected column");
        }
        projection.push_back(*index);
        columns.push_back(column);
    }
    return projection;
}

std::vector<Row> QueryExecutor::collectRows(const Select &command, const Table &table,
                                            const QueryPlan &plan) const {
    auto applyResidual = [&](std::vector<Row> rows) {
        if (!plan.residual) {
            return rows;
        }
        std::vector<Row> filtered;
        filtered.reserve(rows.size());
        for (auto &row : rows) {
            if (matches(row, table, *plan.residual)) {
                filtered.push_back(std::move(row));
            }
        }
        return filtered;
    };

    if (plan.accessPath == AccessPath::HashIndexLookup) {
        if (auto rowIds = table.indexedLookup(plan.indexColumn, plan.indexValue)) {
            return applyResidual(rowsByIdForRead(table, *rowIds));
        }
        return {};
    }
    if (plan.accessPath == AccessPath::OrderedIndexRange) {
        if (auto rowIds =
                table.orderedLookup(plan.indexColumn, plan.indexOp, plan.indexValue)) {
            return applyResidual(rowsByIdForRead(table, *rowIds));
        }
        return {};
    }
    if (plan.accessPath == AccessPath::HashIndexInLookup) {
        std::vector<RowId> combined;
        for (const auto &value : plan.indexValues) {
            if (auto rowIds = table.indexedLookup(plan.indexColumn, value)) {
                combined.insert(combined.end(), rowIds->begin(), rowIds->end());
            }
        }
        return applyResidual(rowsByIdForRead(table, combined));
    }

    std::vector<Row> rows;
    const auto snapshot = rowsSnapshotForRead(table);
    for (const auto &row : snapshot) {
        if (command.where && !matches(row, table, *command.where)) {
            continue;
        }
        rows.push_back(row);
    }
    return rows;
}

QueryResult QueryExecutor::executeJoinSelect(const Select &command) {
    auto leftTable = requireTable(command.table);
    auto rightTable = requireTable(command.join->table);
    const auto leftJoinColumn =
        resolveTableColumn(*leftTable, command.table, command.join->leftColumn);
    const auto rightJoinColumn =
        resolveTableColumn(*rightTable, command.join->table, command.join->rightColumn);
    if (!leftJoinColumn || !rightJoinColumn) {
        throw std::runtime_error("unknown join column");
    }

    std::vector<std::string> joinedColumns;
    for (const auto &column : leftTable->schema()) {
        joinedColumns.push_back(command.table + "." + column.name);
    }
    for (const auto &column : rightTable->schema()) {
        joinedColumns.push_back(command.join->table + "." + column.name);
    }

    std::vector<std::size_t> projection;
    std::vector<std::string> projectedColumns;
    if (command.columns.size() == 1 && command.columns.front() == "*") {
        for (std::size_t i = 0; i < joinedColumns.size(); ++i) {
            projection.push_back(i);
        }
        projectedColumns = joinedColumns;
    } else {
        projection.reserve(command.columns.size());
        projectedColumns.reserve(command.columns.size());
        for (const auto &column : command.columns) {
            auto index = resolveResultColumn(joinedColumns, column);
            if (!index) {
                throw std::runtime_error("unknown selected column");
            }
            projection.push_back(*index);
            projectedColumns.push_back(joinedColumns[*index]);
        }
    }

    const auto leftRows = rowsSnapshotForRead(*leftTable);
    const auto rightRows = rowsSnapshotForRead(*rightTable);
    std::map<Value, std::vector<Row>> rightRowsByKey;
    for (const auto &row : rightRows) {
        rightRowsByKey[row[*rightJoinColumn]].push_back(row);
    }

    const ColumnLookup joinLookup = [&](std::string_view column) {
        return resolveResultColumn(joinedColumns, column);
    };

    std::vector<Row> joinedRows;
    for (const auto &leftRow : leftRows) {
        auto matchingRightRows = rightRowsByKey.find(leftRow[*leftJoinColumn]);
        if (matchingRightRows == rightRowsByKey.end()) {
            continue;
        }
        for (const auto &rightRow : matchingRightRows->second) {
            Row joined;
            joined.reserve(leftRow.size() + rightRow.size());
            joined.insert(joined.end(), leftRow.begin(), leftRow.end());
            joined.insert(joined.end(), rightRow.begin(), rightRow.end());
            if (command.where && !evalPredicate(*command.where, joined, joinLookup)) {
                continue;
            }
            joinedRows.push_back(std::move(joined));
        }
    }

    if (command.orderBy) {
        const auto orderColumn = resolveResultColumn(joinedColumns, command.orderBy->column);
        if (!orderColumn) {
            throw std::runtime_error("unknown ORDER BY column");
        }
        sortRowsByColumn(joinedRows, *orderColumn, command.orderBy->ascending);
    }

    return projectWithLimit(std::move(joinedRows), projection, std::move(projectedColumns),
                            command.limit);
}

bool QueryExecutor::matches(const Row &row, const Table &table, const Predicate &predicate) const {
    return evalPredicate(predicate, row, [&](std::string_view column) {
        return table.columnIndex(column);
    });
}

Select QueryExecutor::prepareSelect(const Select &command, RewriteResult &rewrite) const {
    rewrite = rewriteSelect(command);
    Select prepared = rewrite.query;
    if (prepared.where) {
        prepared.where = materializePredicate(*prepared.where);
    }
    return prepared;
}

Predicate QueryExecutor::materializePredicate(const Predicate &predicate) const {
    if (predicate.kind == Predicate::Kind::And || predicate.kind == Predicate::Kind::Or) {
        return Predicate{predicate.kind, std::make_shared<Predicate>(materializePredicate(*predicate.left)),
                         std::make_shared<Predicate>(materializePredicate(*predicate.right))};
    }
    if (predicate.kind == Predicate::Kind::InSubquery) {
        if (!predicate.subquery) {
            throw std::runtime_error("IN subquery is missing");
        }
        return Predicate{predicate.column, evaluateSubqueryValues(*predicate.subquery)};
    }
    return predicate;
}

std::vector<Value> QueryExecutor::evaluateSubqueryValues(const Select &subquery) const {
    RewriteResult rewrite;
    const Select prepared = prepareSelect(subquery, rewrite);
    if (prepared.join) {
        throw std::runtime_error("JOIN inside IN subquery is not supported");
    }
    auto table = requireTable(prepared.table);
    const auto plan = planPreparedSelect(prepared, *table, rewrite);
    auto rows = collectRows(prepared, *table, plan);

    if (prepared.columns.size() != 1 || prepared.columns.front() == "*") {
        throw std::runtime_error("IN subquery must project exactly one column");
    }
    const auto columnIndex = table->columnIndex(prepared.columns.front());
    if (!columnIndex) {
        throw std::runtime_error("unknown IN subquery projection column");
    }

    if (prepared.orderBy) {
        const auto orderIndex = table->columnIndex(prepared.orderBy->column);
        if (!orderIndex) {
            throw std::runtime_error("unknown ORDER BY column");
        }
        sortRowsByColumn(rows, *orderIndex, prepared.orderBy->ascending);
    }

    std::vector<Value> values;
    values.reserve(rows.size());
    for (const auto &row : rows) {
        values.push_back(row[*columnIndex]);
        if (prepared.limit && values.size() >= *prepared.limit) {
            break;
        }
    }
    return values;
}

QueryPlan QueryExecutor::planPreparedSelect(const Select &command, const Table &table,
                                            const RewriteResult &rewrite) const {
    auto plan = planner_.planSelect(command, table);
    for (const auto &note : rewrite.notes) {
        plan.notes.push_back(note);
    }
    return plan;
}

std::shared_ptr<Table> QueryExecutor::requireTable(std::string_view tableName) const {
    if (!database_) {
        throw std::runtime_error("no active database");
    }
    auto table = database_->table(tableName);
    if (!table) {
        throw std::runtime_error("unknown table");
    }
    return table;
}

std::string QueryExecutor::bindPreparedSql(const ExecutePrepared &command) const {
    std::string sql;
    {
        const auto lock = lockManager_.acquireRead();
        auto prepared = preparedStatements_.find(command.name);
        if (prepared == preparedStatements_.end()) {
            throw std::runtime_error("unknown prepared statement");
        }
        sql = prepared->second;
    }

    std::ostringstream bound;
    std::size_t parameterIndex = 0;
    for (const char character : sql) {
        if (character != '?') {
            bound << character;
            continue;
        }
        if (parameterIndex >= command.parameters.size()) {
            throw std::runtime_error("not enough prepared statement parameters");
        }
        bound << sqlLiteral(command.parameters[parameterIndex++]);
    }
    if (parameterIndex != command.parameters.size()) {
        throw std::runtime_error("too many prepared statement parameters");
    }
    return bound.str();
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

std::vector<Row> QueryExecutor::rowsSnapshotForRead(const Table &table) const {
    return table.rowsSnapshot(readSnapshot(), transactionManager_);
}

std::vector<Row> QueryExecutor::rowsByIdForRead(const Table &table,
                                                std::span<const RowId> rowIds) const {
    return table.rowsById(rowIds, readSnapshot(), transactionManager_);
}

bool QueryExecutor::transactionActive() const noexcept { return activeTransaction_.has_value(); }

QueryResult QueryExecutor::rejectIfTransactionActive(std::string_view action) const {
    if (!transactionActive()) {
        return messageResult(true, {});
    }
    return messageResult(false, std::string(action) + " is not allowed while a transaction is active");
}

} // namespace VertexDB
