#include "VertexDB/execution/select_engine.hpp"

#include "select_engine_scan_detail.hpp"

#include "VertexDB/common/comparison_operator.hpp"
#include "VertexDB/execution/select_helpers.hpp"
#include "VertexDB/execution/subquery_runtime.hpp"

#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

namespace VertexDB {

std::vector<std::pair<RowId, Row>>
SelectEngine::collectVisibleEntries(const Select &command, const Table &table,
                                    const QueryPlan &plan, ExplainAnalyzeStats *stats) const {
    const auto snap = ctx_.readSnapshot();
    select_scan_detail::recordSsiScanPredicates(ctx_.session.transactionManager(), snap.self, table,
                                                command, plan);

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
            if (ctx_.subquery->matches(entry.second, table, *plan.residual(), scope)) {
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
                              : !path.indexColumns.empty()
                                    ? table.indexedLookup(path.indexColumns, path.indexValue)
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
                auto intersection = select_scan_detail::evalIntersectPlan(path, table);
                if (intersection.empty() && !plan.complementaryResidual()) {
                    return {};
                }
                auto out = intersection.empty()
                               ? std::vector<std::pair<RowId, Row>>{}
                               : applyResidual(entriesByIdForRead(table, intersection));
                if (!plan.complementaryResidual()) {
                    return out;
                }
                // Partial nested OR under AND: index Intersect∪Union covers indexable arms;
                // complementary scan adds rows matching outer AND + non-indexable OR arms.
                if (stats) {
                    stats->candidates = out.size();
                }
                std::unordered_set<RowId> seen;
                seen.reserve(out.size() * 2 + 1);
                for (const auto &entry : out) {
                    seen.insert(entry.first);
                }
                for (const auto &[rowId, row] : visibleEntriesForRead(table)) {
                    if (seen.contains(rowId)) {
                        continue;
                    }
                    if (ctx_.subquery->matches(row, table, *plan.complementaryResidual(), scope)) {
                        out.emplace_back(rowId, row);
                    }
                }
                return out;
            } else if constexpr (std::is_same_v<T, UnionPlan>) {
                auto ids = select_scan_detail::evalUnionPlan(path, table);
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
                    if (ctx_.subquery->matches(row, table, *plan.residual(), scope)) {
                        out.emplace_back(rowId, row);
                    }
                }
                return out;
            } else {
                std::vector<std::pair<RowId, Row>> scanned;
                for (const auto &[rowId, row] : visibleEntriesForRead(table)) {
                    if (!command.where ||
                        ctx_.subquery->matches(row, table, *command.where, scope)) {
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
