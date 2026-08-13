#include "VertexDB/execution/select_engine.hpp"

#include "VertexDB/common/comparison_operator.hpp"
#include "VertexDB/execution/select_helpers.hpp"
#include "VertexDB/parser/predicate.hpp"
#include "VertexDB/transaction/transaction_manager.hpp"

#include <algorithm>
#include <iterator>
#include <optional>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

namespace VertexDB {
namespace {

std::vector<RowId> evalBitmapNode(const IndexBitmapNode &node, const Table &table);

// Column SIREAD leaves: comparisons, IN lists, LIKE, and AND/OR of those.
// Regex / subquery / expression / column-column comparisons still need relation membership.
[[nodiscard]] bool canRecordColumnSireads(const Predicate &predicate) {
    return std::visit(
        [](const auto &node) -> bool {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, ComparisonPred>) {
                return !node.rhsColumn && !node.expression;
            } else if constexpr (std::is_same_v<T, AndPred> || std::is_same_v<T, OrPred>) {
                return canRecordColumnSireads(*node.left) && canRecordColumnSireads(*node.right);
            } else if constexpr (std::is_same_v<T, InListPred>) {
                return !node.expression;
            } else if constexpr (std::is_same_v<T, LikePred>) {
                return true;
            } else {
                // RegexPred / InSubqueryPred / ExistsPred
                return false;
            }
        },
        predicate);
}

void recordSsiPredicateFromTree(TransactionManager &txns, TransactionId id,
                                std::string_view relation, const Predicate &predicate) {
    std::visit(
        [&](const auto &node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, ComparisonPred>) {
                if (node.rhsColumn || node.expression) {
                    txns.recordRelationRead(id, relation);
                    return;
                }
                txns.recordPredicateRead(
                    id, SsiPredicate{std::string{relation}, node.column, node.op, node.value,
                                     std::nullopt});
            } else if constexpr (std::is_same_v<T, AndPred>) {
                recordSsiPredicateFromTree(txns, id, relation, *node.left);
                recordSsiPredicateFromTree(txns, id, relation, *node.right);
            } else if constexpr (std::is_same_v<T, OrPred>) {
                // OR of column leaves: record each arm (insert matching any arm conflicts).
                // Mixed with regex/subquery/expression → whole OR falls back to membership.
                if (!canRecordColumnSireads(predicate)) {
                    txns.recordRelationRead(id, relation);
                    return;
                }
                recordSsiPredicateFromTree(txns, id, relation, *node.left);
                recordSsiPredicateFromTree(txns, id, relation, *node.right);
            } else if constexpr (std::is_same_v<T, InListPred>) {
                if (node.expression) {
                    txns.recordRelationRead(id, relation);
                    return;
                }
                for (const auto &value : node.inValues) {
                    txns.recordPredicateRead(
                        id, SsiPredicate{std::string{relation}, node.column,
                                         ComparisonOperator::Equal, value, std::nullopt});
                }
            } else if constexpr (std::is_same_v<T, LikePred>) {
                txns.recordPredicateRead(
                    id, SsiPredicate{std::string{relation}, node.column, std::nullopt, std::nullopt,
                                     node.pattern});
            } else {
                // Regex / subquery: conservative relation membership.
                txns.recordRelationRead(id, relation);
            }
        },
        predicate);
}

void recordSsiProbe(TransactionManager &txns, TransactionId id, std::string_view relation,
                    const IndexEqualityProbe &probe) {
    if (probe.expression) {
        txns.recordRelationRead(id, relation);
        return;
    }
    txns.recordPredicateRead(
        id, SsiPredicate{std::string{relation}, probe.column, ComparisonOperator::Equal,
                         probe.value});
}

void recordSsiBitmapNode(TransactionManager &txns, TransactionId id, std::string_view relation,
                         const IndexBitmapNode &node) {
    if (node.kind == IndexBitmapNode::Kind::Probe) {
        recordSsiProbe(txns, id, relation, node.probe);
        return;
    }
    for (const auto &child : node.children) {
        recordSsiBitmapNode(txns, id, relation, child);
    }
}

void recordSsiScanPredicates(TransactionManager &txns, TransactionId id, const Table &table,
                             const Select &command, const QueryPlan &plan) {
    const std::string_view relation = table.name();
    std::visit(
        [&](const auto &path) {
            using T = std::decay_t<decltype(path)>;
            if constexpr (std::is_same_v<T, HashEqPlan>) {
                if (path.indexExpression) {
                    txns.recordRelationRead(id, relation);
                } else {
                    txns.recordPredicateRead(
                        id, SsiPredicate{std::string{relation}, path.indexColumn,
                                         ComparisonOperator::Equal, path.indexValue});
                }
            } else if constexpr (std::is_same_v<T, OrderedRangePlan>) {
                if (path.indexExpression) {
                    txns.recordRelationRead(id, relation);
                } else {
                    txns.recordPredicateRead(
                        id, SsiPredicate{std::string{relation}, path.indexColumn, path.indexOp,
                                         path.indexValue});
                }
            } else if constexpr (std::is_same_v<T, HashInPlan>) {
                if (path.indexExpression) {
                    txns.recordRelationRead(id, relation);
                } else {
                    for (const auto &value : path.indexValues) {
                        txns.recordPredicateRead(
                            id, SsiPredicate{std::string{relation}, path.indexColumn,
                                             ComparisonOperator::Equal, value});
                    }
                }
            } else if constexpr (std::is_same_v<T, IntersectPlan> || std::is_same_v<T, UnionPlan>) {
                for (const auto &child : path.children) {
                    recordSsiBitmapNode(txns, id, relation, child);
                }
            } else if constexpr (std::is_same_v<T, PrefixLikePlan>) {
                // Prefix LIKE always keeps the LikePred as residual; that residual records a
                // column LIKE SIREAD. Do not also take relation membership here.
            } else {
                // FullScanPlan
                if (command.where) {
                    recordSsiPredicateFromTree(txns, id, relation, *command.where);
                } else {
                    txns.recordRelationRead(id, relation);
                }
            }
        },
        plan.path);
    if (plan.residual()) {
        recordSsiPredicateFromTree(txns, id, relation, *plan.residual());
    }
    if (plan.complementaryResidual()) {
        recordSsiPredicateFromTree(txns, id, relation, *plan.complementaryResidual());
    }
}

std::vector<RowId> evalBitmapNode(const IndexBitmapNode &node, const Table &table);

// Combine children under Intersect or Union without allocating a wrapper node.
std::vector<RowId> evalBitmapChildren(IndexBitmapNode::Kind op,
                                      const std::vector<IndexBitmapNode> &children,
                                      const Table &table) {
    if (children.empty()) {
        return {};
    }

    std::optional<std::vector<RowId>> combined;
    for (const auto &child : children) {
        auto childIds = evalBitmapNode(child, table);
        if (op == IndexBitmapNode::Kind::Intersect) {
            if (childIds.empty()) {
                return {};
            }
            if (!combined) {
                combined = std::move(childIds);
                continue;
            }
            std::vector<RowId> next;
            next.reserve(std::min(combined->size(), childIds.size()));
            std::set_intersection(combined->begin(), combined->end(), childIds.begin(),
                                  childIds.end(), std::back_inserter(next));
            combined = std::move(next);
            if (combined->empty()) {
                return {};
            }
        } else {
            // Union
            if (childIds.empty()) {
                continue;
            }
            if (!combined) {
                combined = std::move(childIds);
                continue;
            }
            std::vector<RowId> next;
            next.reserve(combined->size() + childIds.size());
            std::set_union(combined->begin(), combined->end(), childIds.begin(), childIds.end(),
                           std::back_inserter(next));
            combined = std::move(next);
        }
    }
    return combined ? std::move(*combined) : std::vector<RowId>{};
}

std::vector<RowId> evalBitmapNode(const IndexBitmapNode &node, const Table &table) {
    if (node.kind == IndexBitmapNode::Kind::Probe) {
        auto rowIds = node.probe.expression
                          ? table.indexedLookup(*node.probe.expression, node.probe.value)
                          : table.indexedLookup(node.probe.column, node.probe.value);
        if (!rowIds) {
            return {};
        }
        std::sort(rowIds->begin(), rowIds->end());
        return std::move(*rowIds);
    }

    return evalBitmapChildren(node.kind, node.children, table);
}

std::vector<RowId> evalIntersectPlan(const IntersectPlan &path, const Table &table) {
    return evalBitmapChildren(IndexBitmapNode::Kind::Intersect, path.children, table);
}

std::vector<RowId> evalUnionPlan(const UnionPlan &path, const Table &table) {
    return evalBitmapChildren(IndexBitmapNode::Kind::Union, path.children, table);
}

} // namespace

std::vector<std::pair<RowId, Row>>
SelectEngine::collectVisibleEntries(const Select &command, const Table &table,
                                    const QueryPlan &plan, ExplainAnalyzeStats *stats) const {
    const auto snap = ctx_.readSnapshot();
    recordSsiScanPredicates(ctx_.session.transactionManager(), snap.self, table, command, plan);

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
                auto intersection = evalIntersectPlan(path, table);
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
                    if (matches(row, table, *plan.complementaryResidual(), scope)) {
                        out.emplace_back(rowId, row);
                    }
                }
                return out;
            } else if constexpr (std::is_same_v<T, UnionPlan>) {
                auto ids = evalUnionPlan(path, table);
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
