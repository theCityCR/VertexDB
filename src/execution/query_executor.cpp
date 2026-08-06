#include "VertexDB/execution/query_executor.hpp"

#include "VertexDB/common/string_utils.hpp"
#include "VertexDB/execution/predicate_eval.hpp"
#include "VertexDB/execution/select_helpers.hpp"
#include "VertexDB/execution/sql_literal.hpp"
#include "VertexDB/parser/parser.hpp"
#include "VertexDB/planner/query_planner.hpp"

#include <functional>
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
        table->clearDirtyTracking();
        const RowId rowId = table->insert(row, writerId);
        if (transactionActive()) {
            undoLog_.push(UndoRecord{command.table, UndoKind::Insert, rowId, std::nullopt});
        }
        appendPageImageRedo(*table, command.table);
    }
    return messageResult(true, "inserted " + std::to_string(command.rows.size()) + " row(s)");
}

QueryResult QueryExecutor::executeSelect(const Select &command) {
    RewriteResult rewrite;
    const Select prepared = prepareSelect(command, rewrite);
    std::unordered_map<std::string, std::shared_ptr<Table>> temps;
    for (const auto &[name, body] : rewrite.materialize) {
        temps.emplace(name, materializeCteTable(name, body));
    }
    if (!prepared.joins.empty()) {
        return executeJoinSelect(prepared);
    }

    auto table = requireTable(prepared.table, temps);
    const auto plan = planPreparedSelect(prepared, *table, rewrite);

    std::vector<std::string> sourceColumns;
    for (const auto &column : table->schema()) {
        sourceColumns.push_back(column.name);
    }
    auto rows = collectRows(prepared, *table, plan);
    return finalizeSelectResult(prepared, std::move(sourceColumns), std::move(rows));
}

QueryResult QueryExecutor::executeExplain(const ExplainQuery &command) {
    RewriteResult rewrite;
    const Select prepared = prepareSelect(command.query, rewrite);
    std::unordered_map<std::string, std::shared_ptr<Table>> temps;
    for (const auto &[name, body] : rewrite.materialize) {
        temps.emplace(name, materializeCteTable(name, body));
    }

    QueryResult result;
    result.success = true;
    result.message = "explain";
    result.columns = {"plan"};

    if (!prepared.joins.empty()) {
        auto leftTable = requireTable(prepared.table, temps);
        std::size_t leftRows = leftTable->rowCount();
        for (std::size_t joinIndex = 0; joinIndex < prepared.joins.size(); ++joinIndex) {
            const auto &join = prepared.joins[joinIndex];
            auto rightTable = requireTable(join.table, temps);
            JoinPlan joinPlan;
            if (joinIndex == 0) {
                joinPlan = planner_.planJoin(*leftTable, *rightTable, join);
            } else {
                joinPlan = planner_.planJoinAgainstRows(leftRows, *rightTable, join);
            }
            result.rows.push_back({Value{formatJoinPlanExplanation(joinPlan)}});
            leftRows = joinPlan.estimatedRows;
        }
        for (const auto &note : rewrite.notes) {
            result.rows.push_back({Value{note}});
        }
    } else {
        auto table = requireTable(prepared.table, temps);
        const auto plan = planPreparedSelect(prepared, *table, rewrite);
        result.rows.push_back({Value{formatPlanExplanation(plan)}});
    }

    if (hasAggregates(prepared.columns) || !prepared.groupBy.empty()) {
        result.rows.push_back({Value{std::string{"aggregation"}}});
    }
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

std::vector<std::size_t> QueryExecutor::resolveProjection(const Select &command, const Table &table,
                                                          std::vector<std::string> &columns) const {
    std::vector<std::string> sourceColumns;
    for (const auto &column : table.schema()) {
        sourceColumns.push_back(column.name);
    }
    return resolveProjectionFromNames(command, sourceColumns, columns);
}

std::vector<std::size_t>
QueryExecutor::resolveProjectionFromNames(const Select &command,
                                          const std::vector<std::string> &sourceColumns,
                                          std::vector<std::string> &projectedColumns) const {
    std::vector<std::size_t> projection;
    if (isStarProjection(command.columns)) {
        for (std::size_t i = 0; i < sourceColumns.size(); ++i) {
            projection.push_back(i);
            projectedColumns.push_back(sourceColumns[i]);
        }
        return projection;
    }

    for (const auto &expr : command.columns) {
        if (expr.kind != SelectExpr::Kind::Column) {
            throw std::runtime_error("non-aggregate projection expected a column reference");
        }
        auto index = resolveResultColumn(sourceColumns, expr.column);
        if (!index) {
            throw std::runtime_error("unknown selected column");
        }
        projection.push_back(*index);
        projectedColumns.push_back(sourceColumns[*index]);
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
        if (plan.indexExpression) {
            if (auto rowIds = table.indexedLookup(*plan.indexExpression, plan.indexValue)) {
                return applyResidual(rowsByIdForRead(table, *rowIds));
            }
            return {};
        }
        if (auto rowIds = table.indexedLookup(plan.indexColumn, plan.indexValue)) {
            return applyResidual(rowsByIdForRead(table, *rowIds));
        }
        return {};
    }
    if (plan.accessPath == AccessPath::OrderedIndexRange) {
        if (plan.indexExpression) {
            if (auto rowIds = table.orderedLookup(*plan.indexExpression, plan.indexOp,
                                                  plan.indexValue)) {
                return applyResidual(rowsByIdForRead(table, *rowIds));
            }
            return {};
        }
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

QueryResult QueryExecutor::finalizeSelectResult(const Select &command,
                                                std::vector<std::string> sourceColumns,
                                                std::vector<Row> rows) const {
    if (hasAggregates(command.columns) || !command.groupBy.empty()) {
        auto result = aggregateRows(command, sourceColumns, std::move(rows));
        if (command.orderBy) {
            const auto orderColumn = resolveResultColumn(result.columns, command.orderBy->column);
            if (!orderColumn) {
                throw std::runtime_error("unknown ORDER BY column");
            }
            sortRowsByColumn(result.rows, *orderColumn, command.orderBy->ascending);
        }
        if (command.limit && result.rows.size() > *command.limit) {
            result.rows.resize(*command.limit);
        }
        return result;
    }

    std::vector<std::string> projectedColumns;
    const auto projection = resolveProjectionFromNames(command, sourceColumns, projectedColumns);
    if (command.orderBy) {
        const auto orderColumn = resolveResultColumn(sourceColumns, command.orderBy->column);
        if (!orderColumn) {
            throw std::runtime_error("unknown ORDER BY column");
        }
        sortRowsByColumn(rows, *orderColumn, command.orderBy->ascending);
    }
    return projectWithLimit(std::move(rows), projection, std::move(projectedColumns), command.limit);
}

void QueryExecutor::collectJoinRows(const Select &command, std::vector<std::string> &joinedColumns,
                                    std::vector<Row> &joinedRows) const {
    if (command.joins.empty()) {
        throw std::runtime_error("collectJoinRows requires at least one join");
    }

    auto leftTable = requireTable(command.table);
    joinedColumns.clear();
    for (const auto &column : leftTable->schema()) {
        joinedColumns.push_back(command.table + "." + column.name);
    }
    joinedRows = rowsSnapshotForRead(*leftTable);

    for (std::size_t joinIndex = 0; joinIndex < command.joins.size(); ++joinIndex) {
        const auto &join = command.joins[joinIndex];
        auto rightTable = requireTable(join.table);
        const auto leftJoinColumn = resolveResultColumn(joinedColumns, join.leftColumn);
        const auto rightJoinColumn =
            resolveTableColumn(*rightTable, join.table, join.rightColumn);
        if (!leftJoinColumn || !rightJoinColumn) {
            throw std::runtime_error("unknown join column");
        }

        JoinPlan joinPlan;
        if (joinIndex == 0) {
            joinPlan = planner_.planJoin(*leftTable, *rightTable, join);
        } else {
            joinPlan = planner_.planJoinAgainstRows(joinedRows.size(), *rightTable, join);
        }

        std::vector<std::string> nextColumns = joinedColumns;
        for (const auto &column : rightTable->schema()) {
            nextColumns.push_back(join.table + "." + column.name);
        }

        std::vector<Row> nextRows;
        auto appendJoined = [&](const Row &leftRow, const Row &rightRow) {
            Row joined;
            joined.reserve(leftRow.size() + rightRow.size());
            joined.insert(joined.end(), leftRow.begin(), leftRow.end());
            joined.insert(joined.end(), rightRow.begin(), rightRow.end());
            nextRows.push_back(std::move(joined));
        };

        auto unqualified = [](const std::string &name) {
            const auto dot = name.rfind('.');
            return dot == std::string::npos ? name : name.substr(dot + 1);
        };

        if (joinPlan.algorithm == JoinAlgorithm::NestedLoopIndexProbe && joinPlan.outerIsLeft) {
            const auto probeColumn = unqualified(join.rightColumn);
            for (const auto &leftRow : joinedRows) {
                if (auto rowIds =
                        rightTable->indexedLookup(probeColumn, leftRow[*leftJoinColumn])) {
                    for (const auto &rightRow : rowsByIdForRead(*rightTable, *rowIds)) {
                        appendJoined(leftRow, rightRow);
                    }
                }
            }
        } else if (joinPlan.algorithm == JoinAlgorithm::NestedLoopIndexProbe && !joinPlan.outerIsLeft &&
                   joinIndex == 0) {
            const auto outerRows = rowsSnapshotForRead(*rightTable);
            const auto probeColumn = unqualified(join.leftColumn);
            for (const auto &rightRow : outerRows) {
                if (auto rowIds = leftTable->indexedLookup(probeColumn, rightRow[*rightJoinColumn])) {
                    for (const auto &leftRow : rowsByIdForRead(*leftTable, *rowIds)) {
                        appendJoined(leftRow, rightRow);
                    }
                }
            }
        } else {
            const auto rightRows = rowsSnapshotForRead(*rightTable);
            std::map<Value, std::vector<Row>> rightRowsByKey;
            for (const auto &row : rightRows) {
                rightRowsByKey[row[*rightJoinColumn]].push_back(row);
            }
            for (const auto &leftRow : joinedRows) {
                auto matchingRightRows = rightRowsByKey.find(leftRow[*leftJoinColumn]);
                if (matchingRightRows == rightRowsByKey.end()) {
                    continue;
                }
                for (const auto &rightRow : matchingRightRows->second) {
                    appendJoined(leftRow, rightRow);
                }
            }
        }

        joinedColumns = std::move(nextColumns);
        joinedRows = std::move(nextRows);
    }

    if (command.where) {
        const ColumnLookup joinLookup = [&](std::string_view column) {
            return resolveResultColumn(joinedColumns, column);
        };
        std::vector<Row> filtered;
        filtered.reserve(joinedRows.size());
        for (auto &row : joinedRows) {
            if (evalPredicate(*command.where, row, joinLookup)) {
                filtered.push_back(std::move(row));
            }
        }
        joinedRows = std::move(filtered);
    }
}

QueryResult QueryExecutor::executeJoinSelect(const Select &command) {
    std::vector<std::string> joinedColumns;
    std::vector<Row> joinedRows;
    collectJoinRows(command, joinedColumns, joinedRows);
    return finalizeSelectResult(command, std::move(joinedColumns), std::move(joinedRows));
}

bool QueryExecutor::matches(const Row &row, const Table &table, const Predicate &predicate) const {
    if (predicate.kind == Predicate::Kind::And) {
        return matches(row, table, *predicate.left) && matches(row, table, *predicate.right);
    }
    if (predicate.kind == Predicate::Kind::Or) {
        return matches(row, table, *predicate.left) || matches(row, table, *predicate.right);
    }
    if (predicate.kind == Predicate::Kind::Exists) {
        if (!predicate.subquery) {
            throw std::runtime_error("EXISTS subquery is missing");
        }
        if (predicate.referencesOuter || predicate.subquery->hasOuterRefs) {
            const Select bound = bindOuterReferences(*predicate.subquery, row, table);
            return evaluateExists(bound);
        }
        return evaluateExists(*predicate.subquery);
    }
    if (predicate.kind == Predicate::Kind::InSubquery) {
        if (!predicate.subquery) {
            throw std::runtime_error("IN subquery is missing");
        }
        std::vector<Value> values;
        if (predicate.referencesOuter || predicate.subquery->hasOuterRefs) {
            const Select bound = bindOuterReferences(*predicate.subquery, row, table);
            values = evaluateSubqueryValues(bound);
        } else {
            values = evaluateSubqueryValues(*predicate.subquery);
        }
        const auto index = table.columnIndex(predicate.column);
        if (!index) {
            throw std::runtime_error("unknown predicate column");
        }
        for (const auto &value : values) {
            if (row[*index] == value) {
                return true;
            }
        }
        return false;
    }
    return evalPredicate(predicate, row, [&](std::string_view column) {
        auto index = table.columnIndex(column);
        if (index) {
            return index;
        }
        // Allow table.column against the current table schema.
        const auto dot = column.find('.');
        if (dot != std::string_view::npos &&
            equalsIgnoreCase(column.substr(0, dot), table.name())) {
            return table.columnIndex(column.substr(dot + 1));
        }
        return std::optional<std::size_t>{};
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
    if (predicate.kind == Predicate::Kind::Exists) {
        return predicate;
    }
    if (predicate.kind == Predicate::Kind::InSubquery) {
        if (!predicate.subquery) {
            throw std::runtime_error("IN subquery is missing");
        }
        if (predicate.referencesOuter || predicate.subquery->hasOuterRefs) {
            return predicate;
        }
        return Predicate{predicate.column, evaluateSubqueryValues(*predicate.subquery)};
    }
    return predicate;
}

std::vector<Value> QueryExecutor::evaluateSubqueryValues(const Select &subquery) const {
    RewriteResult rewrite;
    const Select prepared = prepareSelect(subquery, rewrite);
    if (!prepared.joins.empty()) {
        throw std::runtime_error("JOIN inside IN subquery is not supported");
    }
    auto table = requireTable(prepared.table);
    const auto plan = planPreparedSelect(prepared, *table, rewrite);
    auto rows = collectRows(prepared, *table, plan);

    if (prepared.columns.size() != 1 || isStarProjection(prepared.columns) ||
        prepared.columns.front().kind != SelectExpr::Kind::Column) {
        throw std::runtime_error("IN subquery must project exactly one column");
    }
    const auto columnIndex = table->columnIndex(prepared.columns.front().column);
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

bool QueryExecutor::evaluateExists(const Select &subquery) const {
    RewriteResult rewrite;
    Select prepared = prepareSelect(subquery, rewrite);
    if (!prepared.joins.empty()) {
        throw std::runtime_error("JOIN inside EXISTS subquery is not supported");
    }
    prepared.limit = 1;
    auto table = requireTable(prepared.table);
    const auto plan = planPreparedSelect(prepared, *table, rewrite);
    auto rows = collectRows(prepared, *table, plan);
    return !rows.empty();
}

namespace {

[[nodiscard]] std::string_view unqualifiedName(std::string_view name) {
    const auto dot = name.rfind('.');
    if (dot == std::string_view::npos) {
        return name;
    }
    return name.substr(dot + 1);
}

[[nodiscard]] Value outerColumnValue(std::string_view column, const Row &outerRow,
                                     const Table &outerTable) {
    auto index = outerTable.columnIndex(unqualifiedName(column));
    if (!index) {
        throw std::runtime_error("unknown outer reference column");
    }
    return outerRow[*index];
}

} // namespace

Predicate QueryExecutor::bindOuterReferences(const Predicate &predicate, const Row &outerRow,
                                             const Table &outerTable) const {
    if (predicate.kind == Predicate::Kind::And || predicate.kind == Predicate::Kind::Or) {
        return Predicate{predicate.kind,
                         std::make_shared<Predicate>(
                             bindOuterReferences(*predicate.left, outerRow, outerTable)),
                         std::make_shared<Predicate>(
                             bindOuterReferences(*predicate.right, outerRow, outerTable))};
    }
    if (predicate.kind == Predicate::Kind::InSubquery || predicate.kind == Predicate::Kind::Exists) {
        throw std::runtime_error("multi-level correlated subqueries are not supported");
    }
    if (predicate.kind != Predicate::Kind::Comparison) {
        return predicate;
    }
    Predicate bound = predicate;
    bound.referencesOuter = false;
    if (predicate.rhsColumn) {
        bound.rhsColumn.reset();
        bound.value = outerColumnValue(*predicate.rhsColumn, outerRow, outerTable);
    }
    return bound;
}

Select QueryExecutor::bindOuterReferences(const Select &subquery, const Row &outerRow,
                                          const Table &outerTable) const {
    Select bound = subquery;
    bound.hasOuterRefs = false;
    if (bound.where) {
        bound.where = bindOuterReferences(*bound.where, outerRow, outerTable);
    }
    return bound;
}

std::shared_ptr<Table> QueryExecutor::materializeCteTable(const std::string &name,
                                                          const Select &body) const {
    RewriteResult rewrite;
    const Select prepared = prepareSelect(body, rewrite);
    QueryResult bodyResult;
    if (!prepared.joins.empty()) {
        bodyResult = const_cast<QueryExecutor *>(this)->executeJoinSelect(prepared);
    } else {
        auto source = requireTable(prepared.table);
        const auto plan = planPreparedSelect(prepared, *source, rewrite);
        std::vector<std::string> sourceColumns;
        for (const auto &column : source->schema()) {
            sourceColumns.push_back(column.name);
        }
        auto rows = collectRows(prepared, *source, plan);
        bodyResult = finalizeSelectResult(prepared, std::move(sourceColumns), std::move(rows));
        if (!bodyResult.success) {
            throw std::runtime_error(bodyResult.message);
        }
        std::vector<Column> schema;
        schema.reserve(bodyResult.columns.size());
        if (hasAggregates(prepared.columns) || !prepared.groupBy.empty()) {
            for (std::size_t i = 0; i < bodyResult.columns.size(); ++i) {
                ColumnType type = ColumnType::Int;
                for (const auto &row : bodyResult.rows) {
                    if (!row[i].isNull()) {
                        type = row[i].type();
                        break;
                    }
                }
                schema.push_back({bodyResult.columns[i], type, true});
            }
        } else {
            std::vector<std::string> projectedNames;
            const auto projection = resolveProjection(prepared, *source, projectedNames);
            schema.reserve(projection.size());
            for (std::size_t i = 0; i < projection.size(); ++i) {
                const auto &sourceColumn = source->schema()[projection[i]];
                schema.push_back({bodyResult.columns[i], sourceColumn.type, sourceColumn.nullable});
            }
        }
        if (schema.empty()) {
            throw std::runtime_error("materialized CTE produced no columns");
        }
        auto table = std::make_shared<Table>(name, std::move(schema));
        for (auto &row : bodyResult.rows) {
            table->insert(std::move(row));
        }
        for (const auto &column : table->schema()) {
            (void)table->createIndex(std::string{"idx_"} + column.name, column.name);
        }
        return table;
    }

    if (!bodyResult.success) {
        throw std::runtime_error(bodyResult.message);
    }
    std::vector<Column> schema;
    schema.reserve(bodyResult.columns.size());
    for (std::size_t i = 0; i < bodyResult.columns.size(); ++i) {
        ColumnType type = ColumnType::Int;
        for (const auto &row : bodyResult.rows) {
            if (!row[i].isNull()) {
                type = row[i].type();
                break;
            }
        }
        schema.push_back({bodyResult.columns[i], type, true});
    }
    if (schema.empty()) {
        throw std::runtime_error("materialized CTE produced no columns");
    }
    auto table = std::make_shared<Table>(name, std::move(schema));
    for (auto &row : bodyResult.rows) {
        table->insert(std::move(row));
    }
    for (const auto &column : table->schema()) {
        (void)table->createIndex(std::string{"idx_"} + column.name, column.name);
    }
    return table;
}

QueryPlan QueryExecutor::planPreparedSelect(const Select &command, const Table &table,
                                            const RewriteResult &rewrite) const {
    auto plan = planner_.planSelect(command, table);
    for (const auto &note : rewrite.notes) {
        plan.notes.push_back(note);
    }
    return plan;
}

std::shared_ptr<Table> QueryExecutor::requireTable(
    std::string_view tableName,
    const std::unordered_map<std::string, std::shared_ptr<Table>> &temps) const {
    for (const auto &[name, table] : temps) {
        if (equalsIgnoreCase(name, tableName)) {
            return table;
        }
    }
    if (!database_) {
        throw std::runtime_error("no active database");
    }
    auto table = database_->table(tableName);
    if (!table) {
        throw std::runtime_error("unknown table");
    }
    return table;
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
