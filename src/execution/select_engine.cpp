#include "VertexDB/execution/query_executor.hpp"

#include "VertexDB/common/string_utils.hpp"
#include "VertexDB/execution/predicate_eval.hpp"
#include "VertexDB/execution/select_aggregate.hpp"
#include "VertexDB/execution/select_helpers.hpp"
#include "VertexDB/planner/query_planner.hpp"

#include <algorithm>
#include <iterator>
#include <map>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace VertexDB {

QueryResult QueryExecutor::executeSelect(const Select &command) {
    return selectEngine_.execute(command);
}

QueryResult QueryExecutor::executeExplain(const ExplainQuery &command) {
    return selectEngine_.explain(command);
}

bool QueryExecutor::matches(const Row &row, const Table &table, const Predicate &predicate) const {
    return selectEngine_.matches(row, table, predicate);
}

std::shared_ptr<Table> QueryExecutor::requireTable(
    std::string_view tableName,
    const std::unordered_map<std::string, std::shared_ptr<Table>> &temps) const {
    return selectEngine_.requireTable(tableName, temps);
}

SelectEngine::SelectEngine(QueryExecutor &owner) noexcept : owner_(owner) {}

QueryResult SelectEngine::execute(const Select &command) {
    RewriteResult rewrite;
    const Select prepared = owner_.subqueryRuntime_.prepareSelect(command, rewrite);
    std::unordered_map<std::string, std::shared_ptr<Table>> temps;
    for (const auto &[name, body] : rewrite.materialize) {
        temps.emplace(name, owner_.subqueryRuntime_.materializeCteTable(name, body));
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

QueryResult SelectEngine::explain(const ExplainQuery &command) {
    RewriteResult rewrite;
    const Select prepared = owner_.subqueryRuntime_.prepareSelect(command.query, rewrite);
    std::unordered_map<std::string, std::shared_ptr<Table>> temps;
    for (const auto &[name, body] : rewrite.materialize) {
        temps.emplace(name, owner_.subqueryRuntime_.materializeCteTable(name, body));
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
                joinPlan = owner_.planner_.planJoin(*leftTable, *rightTable, join);
            } else {
                joinPlan = owner_.planner_.planJoinAgainstRows(leftRows, *rightTable, join);
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

std::vector<Row> SelectEngine::collectRows(const Select &command, const Table &table,
                                           const QueryPlan &plan) const {
    const std::string_view scope = selectScopeName(command);
    auto applyResidual = [&](std::vector<Row> rows) {
        if (!plan.residual()) {
            return rows;
        }
        std::vector<Row> filtered;
        filtered.reserve(rows.size());
        for (auto &row : rows) {
            if (matches(row, table, *plan.residual(), scope)) {
                filtered.push_back(std::move(row));
            }
        }
        return filtered;
    };

    return std::visit(
        [&](const auto &path) -> std::vector<Row> {
            using T = std::decay_t<decltype(path)>;
            if constexpr (std::is_same_v<T, HashEqPlan>) {
                auto rowIds = path.indexExpression
                                  ? table.indexedLookup(*path.indexExpression, path.indexValue)
                                  : table.indexedLookup(path.indexColumn, path.indexValue);
                return rowIds ? applyResidual(rowsByIdForRead(table, *rowIds))
                              : std::vector<Row>{};
            } else if constexpr (std::is_same_v<T, OrderedRangePlan>) {
                auto rowIds =
                    path.indexExpression
                        ? table.orderedLookup(*path.indexExpression, path.indexOp, path.indexValue)
                        : table.orderedLookup(path.indexColumn, path.indexOp, path.indexValue);
                return rowIds ? applyResidual(rowsByIdForRead(table, *rowIds))
                              : std::vector<Row>{};
            } else if constexpr (std::is_same_v<T, HashInPlan>) {
                std::vector<RowId> combined;
                for (const auto &value : path.indexValues) {
                    auto rowIds = path.indexExpression
                                      ? table.indexedLookup(*path.indexExpression, value)
                                      : table.indexedLookup(path.indexColumn, value);
                    if (rowIds) {
                        combined.insert(combined.end(), rowIds->begin(), rowIds->end());
                    }
                }
                return applyResidual(rowsByIdForRead(table, combined));
            } else if constexpr (std::is_same_v<T, PrefixLikePlan>) {
                std::vector<RowId> combined;
                if (auto exact = table.indexedLookup(path.indexColumn, Value{path.prefix})) {
                    combined.insert(combined.end(), exact->begin(), exact->end());
                }
                if (auto greater =
                        table.orderedLookup(path.indexColumn, ComparisonOperator::Greater,
                                            Value{path.prefix})) {
                    combined.insert(combined.end(), greater->begin(), greater->end());
                }
                return applyResidual(rowsByIdForRead(table, combined));
            } else if constexpr (std::is_same_v<T, IntersectPlan>) {
                if (path.intersectProbes.empty()) {
                    return {};
                }
                std::optional<std::vector<RowId>> intersection;
                for (const auto &probe : path.intersectProbes) {
                    auto rowIds = probe.expression
                                      ? table.indexedLookup(*probe.expression, probe.value)
                                      : table.indexedLookup(probe.column, probe.value);
                    if (!rowIds) {
                        return {};
                    }
                    std::sort(rowIds->begin(), rowIds->end());
                    if (!intersection) {
                        intersection = std::move(*rowIds);
                        continue;
                    }
                    std::vector<RowId> next;
                    next.reserve(std::min(intersection->size(), rowIds->size()));
                    std::set_intersection(intersection->begin(), intersection->end(),
                                          rowIds->begin(), rowIds->end(),
                                          std::back_inserter(next));
                    intersection = std::move(next);
                    if (intersection->empty()) {
                        return {};
                    }
                }
                return applyResidual(rowsByIdForRead(table, *intersection));
            } else if constexpr (std::is_same_v<T, UnionPlan>) {
                if (path.unionProbes.empty()) {
                    return {};
                }
                std::optional<std::vector<RowId>> unified;
                for (const auto &probe : path.unionProbes) {
                    auto rowIds = probe.expression
                                      ? table.indexedLookup(*probe.expression, probe.value)
                                      : table.indexedLookup(probe.column, probe.value);
                    if (!rowIds) {
                        continue;
                    }
                    std::sort(rowIds->begin(), rowIds->end());
                    if (!unified) {
                        unified = std::move(*rowIds);
                        continue;
                    }
                    std::vector<RowId> next;
                    next.reserve(unified->size() + rowIds->size());
                    std::set_union(unified->begin(), unified->end(), rowIds->begin(), rowIds->end(),
                                   std::back_inserter(next));
                    unified = std::move(next);
                }
                std::vector<RowId> ids = unified ? std::move(*unified) : std::vector<RowId>{};
                if (!plan.residual()) {
                    return ids.empty() ? std::vector<Row>{} : rowsByIdForRead(table, ids);
                }
                // Residual OR: index union covers indexable arms; complementary scan adds rows
                // matching only the non-indexable residual (AND-style applyResidual would be wrong).
                std::unordered_set<RowId> seen(ids.begin(), ids.end());
                for (const auto &[rowId, row] : visibleEntriesForRead(table)) {
                    if (seen.contains(rowId)) {
                        continue;
                    }
                    if (matches(row, table, *plan.residual(), scope)) {
                        ids.push_back(rowId);
                    }
                }
                return ids.empty() ? std::vector<Row>{} : rowsByIdForRead(table, ids);
            } else {
                std::vector<Row> rows;
                const auto snapshot = rowsSnapshotForRead(table);
                for (const auto &row : snapshot) {
                    if (!command.where || matches(row, table, *command.where, scope)) {
                        rows.push_back(row);
                    }
                }
                return rows;
            }
        },
        plan.path);
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

void SelectEngine::collectJoinRows(const Select &command, std::vector<std::string> &joinedColumns,
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
        const std::optional<std::string_view> rightAlias =
            join.tableAlias ? std::optional<std::string_view>{*join.tableAlias} : std::nullopt;
        const auto rightJoinColumn =
            resolveTableColumn(*rightTable, join.table, join.rightColumn, rightAlias);
        if (!leftJoinColumn || !rightJoinColumn) {
            throw std::runtime_error("unknown join column");
        }

        JoinPlan joinPlan;
        if (joinIndex == 0) {
            joinPlan = owner_.planner_.planJoin(*leftTable, *rightTable, join);
        } else {
            joinPlan = owner_.planner_.planJoinAgainstRows(joinedRows.size(), *rightTable, join);
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
        auto appendLeftOnly = [&](const Row &leftRow) {
            Row joined = leftRow;
            joined.resize(leftRow.size() + rightTable->schema().size(), Value{});
            nextRows.push_back(std::move(joined));
        };

        auto unqualified = [](const std::string &name) {
            const auto dot = name.rfind('.');
            return dot == std::string::npos ? name : name.substr(dot + 1);
        };

        const bool leftOuter = join.kind == JoinKind::LeftOuter;
        const bool equi = join.op == ComparisonOperator::Equal;

        if (equi && joinPlan.algorithm == JoinAlgorithm::NestedLoopIndexProbe &&
            joinPlan.outerIsLeft && !joinPlan.probeColumn.empty()) {
            const auto probeColumn = unqualified(join.rightColumn);
            for (const auto &leftRow : joinedRows) {
                bool matched = false;
                if (auto rowIds =
                        rightTable->indexedLookup(probeColumn, leftRow[*leftJoinColumn])) {
                    for (const auto &rightRow : rowsByIdForRead(*rightTable, *rowIds)) {
                        appendJoined(leftRow, rightRow);
                        matched = true;
                    }
                }
                if (leftOuter && !matched) {
                    appendLeftOnly(leftRow);
                }
            }
        } else if (equi && joinPlan.algorithm == JoinAlgorithm::NestedLoopIndexProbe &&
                   !joinPlan.outerIsLeft && joinIndex == 0 && !leftOuter) {
            const auto outerRows = rowsSnapshotForRead(*rightTable);
            const auto probeColumn = unqualified(join.leftColumn);
            for (const auto &rightRow : outerRows) {
                if (auto rowIds = leftTable->indexedLookup(probeColumn, rightRow[*rightJoinColumn])) {
                    for (const auto &leftRow : rowsByIdForRead(*leftTable, *rowIds)) {
                        appendJoined(leftRow, rightRow);
                    }
                }
            }
        } else if (equi && joinPlan.algorithm == JoinAlgorithm::HashJoin) {
            const auto rightRows = rowsSnapshotForRead(*rightTable);
            std::map<Value, std::vector<Row>> rightRowsByKey;
            for (const auto &row : rightRows) {
                rightRowsByKey[row[*rightJoinColumn]].push_back(row);
            }
            for (const auto &leftRow : joinedRows) {
                auto matchingRightRows = rightRowsByKey.find(leftRow[*leftJoinColumn]);
                if (matchingRightRows == rightRowsByKey.end()) {
                    if (leftOuter) {
                        appendLeftOnly(leftRow);
                    }
                    continue;
                }
                for (const auto &rightRow : matchingRightRows->second) {
                    appendJoined(leftRow, rightRow);
                }
            }
        } else {
            // Non-equi (or left-outer without usable probe): nested-loop compare.
            const auto rightRows = rowsSnapshotForRead(*rightTable);
            for (const auto &leftRow : joinedRows) {
                bool matched = false;
                for (const auto &rightRow : rightRows) {
                    if (compareValues(leftRow[*leftJoinColumn], join.op,
                                      rightRow[*rightJoinColumn])) {
                        appendJoined(leftRow, rightRow);
                        matched = true;
                    }
                }
                if (leftOuter && !matched) {
                    appendLeftOnly(leftRow);
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

QueryResult SelectEngine::executeJoinSelect(const Select &command) {
    std::vector<std::string> joinedColumns;
    std::vector<Row> joinedRows;
    collectJoinRows(command, joinedColumns, joinedRows);
    return finalizeSelectResult(command, std::move(joinedColumns), std::move(joinedRows));
}

bool SelectEngine::matches(const Row &row, const Table &table, const Predicate &predicate,
                           std::string_view scopeName) const {
    const std::string_view scope = scopeName.empty() ? std::string_view{table.name()} : scopeName;
    return std::visit(
        [&](const auto &node) -> bool {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, AndPred>) {
                return matches(row, table, *node.left, scope) &&
                       matches(row, table, *node.right, scope);
            } else if constexpr (std::is_same_v<T, OrPred>) {
                return matches(row, table, *node.left, scope) ||
                       matches(row, table, *node.right, scope);
            } else if constexpr (std::is_same_v<T, ExistsPred>) {
                if (!node.subquery) {
                    throw std::runtime_error("EXISTS subquery is missing");
                }
                if (node.referencesOuter || node.subquery->hasOuterRefs) {
                    const Select bound = owner_.subqueryRuntime_.bindOuterReferences(
                        *node.subquery, row, table, scope);
                    return owner_.subqueryRuntime_.evaluateExists(bound);
                }
                return owner_.subqueryRuntime_.evaluateExists(*node.subquery);
            } else if constexpr (std::is_same_v<T, InSubqueryPred>) {
                if (!node.subquery) {
                    throw std::runtime_error("IN subquery is missing");
                }
                const Select *subquery = node.subquery.get();
                std::optional<Select> bound;
                if (node.referencesOuter || node.subquery->hasOuterRefs) {
                    bound = owner_.subqueryRuntime_.bindOuterReferences(*node.subquery, row, table,
                                                                        scope);
                    subquery = &*bound;
                }
                const auto values = owner_.subqueryRuntime_.evaluateSubqueryValues(*subquery);
                const auto index = table.columnIndex(node.column);
                if (!index) {
                    throw std::runtime_error("unknown predicate column");
                }
                return std::find(values.begin(), values.end(), row[*index]) != values.end();
            } else {
                return evalPredicate(predicate, row, [&](std::string_view column) {
                    auto index = table.columnIndex(column);
                    if (index) {
                        return index;
                    }
                    const auto dot = column.find('.');
                    if (dot != std::string_view::npos) {
                        const auto qual = column.substr(0, dot);
                        if (equalsIgnoreCase(qual, table.name()) ||
                            equalsIgnoreCase(qual, scope)) {
                            return table.columnIndex(column.substr(dot + 1));
                        }
                    }
                    return std::optional<std::size_t>{};
                });
            }
        },
        predicate);
}

QueryPlan SelectEngine::planPreparedSelect(const Select &command, const Table &table,
                                           const RewriteResult &rewrite) const {
    auto plan = owner_.planner_.planSelect(command, table);
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
    if (!owner_.database_) {
        throw std::runtime_error("no active database");
    }
    auto table = owner_.database_->table(tableName);
    if (!table) {
        throw std::runtime_error("unknown table");
    }
    return table;
}

std::vector<Row> SelectEngine::rowsSnapshotForRead(const Table &table) const {
    return table.rowsSnapshot(owner_.readSnapshot(), owner_.session_.transactionManager());
}

std::vector<std::pair<RowId, Row>> SelectEngine::visibleEntriesForRead(const Table &table) const {
    return table.visibleEntries(owner_.readSnapshot(), owner_.session_.transactionManager());
}

std::vector<Row> SelectEngine::rowsByIdForRead(const Table &table,
                                               std::span<const RowId> rowIds) const {
    return table.rowsById(rowIds, owner_.readSnapshot(), owner_.session_.transactionManager());
}

} // namespace VertexDB
