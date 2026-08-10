#include "VertexDB/execution/select_engine.hpp"

#include "VertexDB/common/string_utils.hpp"
#include "VertexDB/execution/select_aggregate.hpp"
#include "VertexDB/execution/select_helpers.hpp"
#include "VertexDB/execution/subquery_runtime.hpp"
#include "VertexDB/planner/query_planner.hpp"
#include "VertexDB/planner/rewriter.hpp"

#include <stdexcept>
#include <utility>

namespace VertexDB {

SelectEngine::SelectEngine(ExecutionContext &ctx) noexcept : ctx_(ctx) {}

QueryResult SelectEngine::execute(const Select &command) {
    RewriteResult rewrite;
    const Select prepared = ctx_.subquery->prepareSelect(command, rewrite);
    std::unordered_map<std::string, std::shared_ptr<Table>> temps;
    for (const auto &cte : rewrite.materialize) {
        temps.emplace(cte.name, ctx_.subquery->materializeCteTable(cte));
    }
    if (!prepared.joins.empty()) {
        return executeJoinSelect(prepared, temps);
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

QueryResult SelectEngine::explain(const ExplainQuery &command) {
    RewriteResult rewrite;
    const Select prepared = ctx_.subquery->prepareSelect(command.query, rewrite);
    std::unordered_map<std::string, std::shared_ptr<Table>> temps;
    for (const auto &cte : rewrite.materialize) {
        temps.emplace(cte.name, ctx_.subquery->materializeCteTable(cte));
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
                joinPlan = ctx_.planner.planJoin(*leftTable, *rightTable, join);
            } else {
                joinPlan = ctx_.planner.planJoinAgainstRows(leftRows, *rightTable, join);
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

std::vector<std::size_t> SelectEngine::resolveProjection(const Select &command, const Table &table,
                                                         std::vector<std::string> &columns) const {
    std::vector<std::string> sourceColumns;
    for (const auto &column : table.schema()) {
        sourceColumns.push_back(column.name);
    }
    return resolveProjectionFromNames(command, sourceColumns, columns);
}

std::vector<std::size_t>
SelectEngine::resolveProjectionFromNames(const Select &command,
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

QueryResult SelectEngine::finalizeSelectResult(const Select &command,
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

bool SelectEngine::matches(const Row &row, const Table &table, const Predicate &predicate,
                           std::string_view scopeName) const {
    return ctx_.subquery->matches(row, table, predicate, scopeName);
}

QueryPlan SelectEngine::planPreparedSelect(const Select &command, const Table &table,
                                           const RewriteResult &rewrite) const {
    auto plan = ctx_.planner.planSelect(command, table);
    for (const auto &note : rewrite.notes) {
        plan.estimates.notes.push_back(note);
    }
    return plan;
}

std::shared_ptr<Table> SelectEngine::requireTable(
    std::string_view tableName,
    const std::unordered_map<std::string, std::shared_ptr<Table>> &temps) const {
    for (const auto &[name, table] : temps) {
        if (equalsIgnoreCase(name, tableName)) {
            return table;
        }
    }
    if (!ctx_.database) {
        throw std::runtime_error("no active database");
    }
    auto table = ctx_.database->table(tableName);
    if (!table) {
        throw std::runtime_error("unknown table");
    }
    return table;
}

std::vector<Row> SelectEngine::rowsSnapshotForRead(const Table &table) const {
    return table.rowsSnapshot(ctx_.readSnapshot(), ctx_.session.transactionManager());
}

std::vector<std::pair<RowId, Row>> SelectEngine::visibleEntriesForRead(const Table &table) const {
    return table.visibleEntries(ctx_.readSnapshot(), ctx_.session.transactionManager());
}

std::vector<Row> SelectEngine::rowsByIdForRead(const Table &table,
                                               std::span<const RowId> rowIds) const {
    return table.rowsById(rowIds, ctx_.readSnapshot(), ctx_.session.transactionManager());
}

} // namespace VertexDB
