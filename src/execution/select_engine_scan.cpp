#include "VertexDB/execution/select_engine.hpp"

#include "VertexDB/common/comparison_operator.hpp"
#include "VertexDB/execution/select_helpers.hpp"
#include "VertexDB/execution/select_scope.hpp"

#include <algorithm>
#include <iterator>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace VertexDB {

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

} // namespace VertexDB
