#include "select_engine_scan_detail.hpp"

#include "VertexDB/common/comparison_operator.hpp"
#include "VertexDB/parser/predicate.hpp"

#include <algorithm>
#include <type_traits>

namespace VertexDB {
namespace select_scan_detail {
namespace {

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
                                         ComparisonOperator::equal, value, std::nullopt});
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
        id, SsiPredicate{std::string{relation}, probe.column, ComparisonOperator::equal,
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

} // namespace

void recordSsiScanPredicates(TransactionManager &txns, TransactionId id, const Table &table,
                             const Select &command, const QueryPlan &plan) {
    const std::string_view relation = table.name();
    std::visit(
        [&](const auto &path) {
            using T = std::decay_t<decltype(path)>;
            if constexpr (std::is_same_v<T, HashEqPlan>) {
                if (path.indexExpression) {
                    txns.recordRelationRead(id, relation);
                } else if (!path.indexColumns.empty()) {
                    const auto &parts = path.indexValue.compositeParts();
                    const std::size_t n =
                        std::min(path.indexColumns.size(), parts.size());
                    for (std::size_t i = 0; i < n; ++i) {
                        txns.recordPredicateRead(
                            id, SsiPredicate{std::string{relation}, path.indexColumns[i],
                                             ComparisonOperator::equal, parts[i]});
                    }
                } else {
                    txns.recordPredicateRead(
                        id, SsiPredicate{std::string{relation}, path.indexColumn,
                                         ComparisonOperator::equal, path.indexValue});
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
                                             ComparisonOperator::equal, value});
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

} // namespace select_scan_detail
} // namespace VertexDB
