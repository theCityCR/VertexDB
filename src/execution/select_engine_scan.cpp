#include "VertexDB/execution/select_engine.hpp"

#include "VertexDB/common/comparison_operator.hpp"
#include "VertexDB/execution/select_helpers.hpp"

#include <algorithm>
#include <iterator>
#include <optional>
#include <unordered_set>
#include <utility>

namespace VertexDB {

std::vector<std::pair<RowId, Row>>
SelectEngine::collectVisibleEntries(const Select &command, const Table &table,
                                    const QueryPlan &plan, ExplainAnalyzeStats *stats) const {
    const std::string_view scope = selectScopeName(command);
    auto applyResidual = [&](std::vector<std::pair<RowId, Row>> entries) {
        if (!plan.residual()) {
            return entries;
        }
        if (stats) {
            stats->candidates = entries.size();
        }
        std::vector<std::pair<RowId, Row>> filtered;
        filtered.reserve(entries.size());
        for (auto &entry : entries) {
            if (matches(entry.second, table, *plan.residual(), scope)) {
                filtered.push_back(std::move(entry));
            }
        }
        return filtered;
    };

    auto entries = std::visit(
        [&](const auto &path) -> std::vector<std::pair<RowId, Row>> {
            using T = std::decay_t<decltype(path)>;
            if constexpr (std::is_same_v<T, HashEqPlan>) {
                auto rowIds = path.indexExpression
                                  ? table.indexedLookup(*path.indexExpression, path.indexValue)
                                  : table.indexedLookup(path.indexColumn, path.indexValue);
                return rowIds ? applyResidual(entriesByIdForRead(table, *rowIds))
                              : std::vector<std::pair<RowId, Row>>{};
            } else if constexpr (std::is_same_v<T, OrderedRangePlan>) {
                auto rowIds =
                    path.indexExpression
                        ? table.orderedLookup(*path.indexExpression, path.indexOp, path.indexValue)
                        : table.orderedLookup(path.indexColumn, path.indexOp, path.indexValue);
                return rowIds ? applyResidual(entriesByIdForRead(table, *rowIds))
                              : std::vector<std::pair<RowId, Row>>{};
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
                return applyResidual(entriesByIdForRead(table, combined));
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
                return applyResidual(entriesByIdForRead(table, combined));
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
                return applyResidual(entriesByIdForRead(table, *intersection));
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
                    return ids.empty() ? std::vector<std::pair<RowId, Row>>{}
                                       : entriesByIdForRead(table, ids);
                }
                // Residual OR: index union covers indexable arms; complementary scan adds rows
                // matching only the non-indexable residual (AND-style applyResidual would be wrong).
                if (stats) {
                    stats->candidates = ids.size();
                }
                std::unordered_set<RowId> seen(ids.begin(), ids.end());
                auto out = ids.empty() ? std::vector<std::pair<RowId, Row>>{}
                                       : entriesByIdForRead(table, ids);
                for (const auto &[rowId, row] : visibleEntriesForRead(table)) {
                    if (seen.contains(rowId)) {
                        continue;
                    }
                    if (matches(row, table, *plan.residual(), scope)) {
                        out.emplace_back(rowId, row);
                    }
                }
                return out;
            } else {
                std::vector<std::pair<RowId, Row>> scanned;
                for (const auto &[rowId, row] : visibleEntriesForRead(table)) {
                    if (!command.where || matches(row, table, *command.where, scope)) {
                        scanned.emplace_back(rowId, row);
                    }
                }
                return scanned;
            }
        },
        plan.path);

    if (stats) {
        stats->actualRows = entries.size();
    }
    return entries;
}

std::vector<Row> SelectEngine::collectRows(const Select &command, const Table &table,
                                           const QueryPlan &plan,
                                           ExplainAnalyzeStats *stats) const {
    auto entries = collectVisibleEntries(command, table, plan, stats);
    std::vector<Row> rows;
    rows.reserve(entries.size());
    for (auto &entry : entries) {
        rows.push_back(std::move(entry.second));
    }
    return rows;
}

} // namespace VertexDB
