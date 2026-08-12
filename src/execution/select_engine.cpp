#include "VertexDB/execution/select_engine.hpp"

#include "VertexDB/common/string_utils.hpp"
#include "VertexDB/execution/select_aggregate.hpp"
#include "VertexDB/execution/select_helpers.hpp"
#include "VertexDB/execution/subquery_runtime.hpp"
#include "VertexDB/planner/query_planner.hpp"
#include "VertexDB/planner/rewriter.hpp"

#include <chrono>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

namespace VertexDB {

SelectEngine::SelectEngine(ExecutionContext &ctx) noexcept : ctx_(ctx) {}

namespace {

[[nodiscard]] Select stripSetOpsAndOrderLimit(Select select) {
    select.setOps.clear();
    select.orderBy = std::nullopt;
    select.limit = std::nullopt;
    return select;
}

[[nodiscard]] QueryResult applyOrderLimitToResult(QueryResult result, const Select &command) {
    if (!result.success) {
        return result;
    }
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

} // namespace

QueryResult SelectEngine::execute(const Select &command) {
    if (!command.setOps.empty()) {
        Select left = stripSetOpsAndOrderLimit(command);
        QueryResult combined = execute(left);
        for (const auto &arm : command.setOps) {
            if (!arm.select) {
                throw std::runtime_error("set operation is missing a SELECT arm");
            }
            Select right = *arm.select;
            // Outer WITH CTEs are visible to every arm.
            if (right.ctes.empty() && !command.ctes.empty()) {
                right.ctes = command.ctes;
            }
            combined = applySetOperation(arm.op, std::move(combined), execute(right));
        }
        return applyOrderLimitToResult(std::move(combined), command);
    }

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
    if (std::holds_alternative<Update>(command.query) ||
        std::holds_alternative<Delete>(command.query)) {
        if (command.analyze) {
            throw std::runtime_error("EXPLAIN ANALYZE does not support UPDATE/DELETE");
        }
        const auto scan = std::visit(
            [](const auto &mutation) -> Select {
                using M = std::decay_t<decltype(mutation)>;
                if constexpr (std::is_same_v<M, Update> || std::is_same_v<M, Delete>) {
                    return mutationScanSelect(mutation.table, mutation.where);
                }
                return Select{};
            },
            command.query);
        auto table = requireTable(scan.table);
        const auto plan = ctx_.planner.planSelect(scan, *table);
        auto text = formatPlanExplanation(plan);
        const char *prefix =
            std::holds_alternative<Update>(command.query) ? "update: " : "delete: ";
        text = std::string{prefix} + text;

        QueryResult result;
        result.success = true;
        result.message = "explain";
        result.columns = {"plan"};
        result.rows.push_back({Value{std::move(text)}});
        return result;
    }

    const Select &selectQuery = std::get<Select>(command.query);

    if (!selectQuery.setOps.empty()) {
        QueryResult result;
        result.success = true;
        result.message = "explain";
        result.columns = {"plan"};
        result.rows.push_back({Value{std::string{"set operation left arm"}}});
        ExplainQuery leftExplain;
        leftExplain.analyze = false;
        Select left = selectQuery;
        left.setOps.clear();
        left.orderBy = std::nullopt;
        left.limit = std::nullopt;
        leftExplain.query = left;
        auto leftPlan = explain(leftExplain);
        for (auto &row : leftPlan.rows) {
            result.rows.push_back(std::move(row));
        }
        for (const auto &arm : selectQuery.setOps) {
            result.rows.push_back(
                {Value{std::string{setOpKindName(arm.op)}}});
            if (!arm.select) {
                continue;
            }
            ExplainQuery rightExplain;
            rightExplain.analyze = false;
            Select right = *arm.select;
            if (right.ctes.empty() && !selectQuery.ctes.empty()) {
                right.ctes = selectQuery.ctes;
            }
            rightExplain.query = right;
            auto rightPlan = explain(rightExplain);
            for (auto &row : rightPlan.rows) {
                result.rows.push_back(std::move(row));
            }
        }
        if (command.analyze) {
            const auto started = std::chrono::steady_clock::now();
            auto executed = execute(selectQuery);
            const auto elapsedMs = std::chrono::duration<double, std::milli>(
                                       std::chrono::steady_clock::now() - started)
                                       .count();
            auto summary = appendExplainAnalyzeActuals(
                std::string{"set operation result"}, executed.rows.size(), std::nullopt,
                elapsedMs);
            result.rows.insert(result.rows.begin(), {Value{std::move(summary)}});
        }
        return result;
    }

    const auto started = std::chrono::steady_clock::now();

    RewriteResult rewrite;
    const Select prepared = ctx_.subquery->prepareSelect(selectQuery, rewrite);
    std::unordered_map<std::string, std::shared_ptr<Table>> temps;
    for (const auto &cte : rewrite.materialize) {
        temps.emplace(cte.name, ctx_.subquery->materializeCteTable(cte));
    }

    QueryResult result;
    result.success = true;
    result.message = "explain";
    result.columns = {"plan"};

    ExplainAnalyzeStats stats;
    ExplainAnalyzeStats *statsPtr = command.analyze ? &stats : nullptr;

    if (!prepared.joins.empty()) {
        auto leftTable = requireTable(prepared.table, temps);
        std::size_t leftRows = leftTable->rowCount();
        std::vector<JoinPlan> joinPlans;
        joinPlans.reserve(prepared.joins.size());
        for (std::size_t joinIndex = 0; joinIndex < prepared.joins.size(); ++joinIndex) {
            const auto &join = prepared.joins[joinIndex];
            auto rightTable = requireTable(join.table, temps);
            JoinPlan joinPlan;
            if (joinIndex == 0) {
                joinPlan = ctx_.planner.planJoin(*leftTable, *rightTable, join);
            } else {
                joinPlan = ctx_.planner.planJoinAgainstRows(leftRows, *rightTable, join);
            }
            leftRows = joinPlan.estimatedRows;
            joinPlans.push_back(std::move(joinPlan));
        }

        if (command.analyze) {
            std::vector<std::string> joinedColumns;
            std::vector<Row> joinedRows;
            collectJoinRows(prepared, joinedColumns, joinedRows, temps, statsPtr);
            if (hasAggregates(prepared.columns) || !prepared.groupBy.empty()) {
                auto aggregated = aggregateRows(prepared, joinedColumns, std::move(joinedRows));
                stats.actualRows = aggregated.rows.size();
                if (!stats.joinActualRows.empty()) {
                    stats.joinActualRows.back() = stats.actualRows;
                }
            }
        }

        const auto elapsedMs = std::chrono::duration<double, std::milli>(
                                   std::chrono::steady_clock::now() - started)
                                   .count();
        for (std::size_t i = 0; i < joinPlans.size(); ++i) {
            auto text = formatJoinPlanExplanation(joinPlans[i]);
            if (command.analyze) {
                const std::size_t actual =
                    i < stats.joinActualRows.size() ? stats.joinActualRows[i] : stats.actualRows;
                const std::optional<double> time =
                    i == 0 ? std::optional<double>{elapsedMs} : std::nullopt;
                text = appendExplainAnalyzeActuals(std::move(text), actual, std::nullopt, time);
            }
            result.rows.push_back({Value{std::move(text)}});
        }
        for (const auto &note : rewrite.notes) {
            result.rows.push_back({Value{note}});
        }
    } else {
        auto table = requireTable(prepared.table, temps);
        const auto plan = planPreparedSelect(prepared, *table, rewrite);
        auto text = formatPlanExplanation(plan);

        if (command.analyze) {
            std::vector<std::string> sourceColumns;
            for (const auto &column : table->schema()) {
                sourceColumns.push_back(column.name);
            }
            auto rows = collectRows(prepared, *table, plan, statsPtr);
            if (hasAggregates(prepared.columns) || !prepared.groupBy.empty()) {
                auto aggregated = aggregateRows(prepared, sourceColumns, std::move(rows));
                stats.actualRows = aggregated.rows.size();
            }
            const auto elapsedMs = std::chrono::duration<double, std::milli>(
                                       std::chrono::steady_clock::now() - started)
                                       .count();
            text = appendExplainAnalyzeActuals(std::move(text), stats.actualRows, stats.candidates,
                                               elapsedMs);
        }
        result.rows.push_back({Value{std::move(text)}});
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

std::vector<std::pair<RowId, Row>>
SelectEngine::entriesByIdForRead(const Table &table, std::span<const RowId> rowIds) const {
    return table.visibleEntriesById(rowIds, ctx_.readSnapshot(), ctx_.session.transactionManager());
}

} // namespace VertexDB
