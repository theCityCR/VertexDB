#include "VertexDB/execution/subquery_runtime.hpp"

#include "VertexDB/common/string_utils.hpp"
#include "VertexDB/execution/predicate_eval.hpp"
#include "VertexDB/execution/select_engine.hpp"
#include "VertexDB/execution/select_helpers.hpp"
#include "VertexDB/execution/select_scope.hpp"
#include "VertexDB/parser/predicate.hpp"

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace VertexDB {

SubqueryRuntime::SubqueryRuntime(ExecutionContext &ctx) noexcept : ctx_(ctx) {}

Select SubqueryRuntime::prepareSelect(const Select &command, RewriteResult &rewrite) const {
    rewrite = rewriteSelect(command);
    Select prepared = rewrite.query;
    normalizeSelectScopeQualifiers(prepared);
    if (prepared.where) {
        prepared.where = materializePredicate(*prepared.where);
    }
    return prepared;
}

Predicate SubqueryRuntime::materializePredicate(const Predicate &predicate) const {
    return std::visit(
        [&](const auto &node) -> Predicate {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, AndPred>) {
                return makeAnd(materializePredicate(*node.left),
                               materializePredicate(*node.right));
            } else if constexpr (std::is_same_v<T, OrPred>) {
                return makeOr(materializePredicate(*node.left),
                              materializePredicate(*node.right));
            } else if constexpr (std::is_same_v<T, InSubqueryPred>) {
                if (!node.subquery) {
                    throw std::runtime_error("IN subquery is missing");
                }
                if (node.referencesOuter || node.subquery->hasOuterRefs) {
                    return node;
                }
                return makeInList(node.column, evaluateSubqueryValues(*node.subquery));
            } else {
                return node;
            }
        },
        predicate);
}

std::vector<Value> SubqueryRuntime::evaluateSubqueryValues(const Select &subquery) const {
    if (!subquery.setOps.empty()) {
        auto result = evaluateSelectResult(subquery);
        if (result.columns.size() != 1) {
            throw std::runtime_error("IN subquery must project exactly one column");
        }
        std::vector<Value> values;
        values.reserve(result.rows.size());
        for (const auto &row : result.rows) {
            values.push_back(row[0]);
        }
        return values;
    }

    RewriteResult rewrite;
    const Select prepared = prepareSelect(subquery, rewrite);
    std::unordered_map<std::string, std::shared_ptr<Table>> temps;
    materializeCteList(rewrite.materialize, temps);

    if (prepared.columns.size() != 1 || isStarProjection(prepared.columns) ||
        prepared.columns.front().kind != SelectExpr::Kind::Column) {
        throw std::runtime_error("IN subquery must project exactly one column");
    }

    if (!prepared.joins.empty()) {
        auto result = ctx_.select->executeJoinSelect(prepared, temps);
        if (!result.success) {
            throw std::runtime_error(result.message);
        }
        if (result.columns.size() != 1) {
            throw std::runtime_error("IN subquery must project exactly one column");
        }
        std::vector<Value> values;
        values.reserve(result.rows.size());
        for (const auto &row : result.rows) {
            values.push_back(row[0]);
            if (prepared.limit && values.size() >= *prepared.limit) {
                break;
            }
        }
        return values;
    }

    auto table = ctx_.select->requireTable(prepared.table, temps);
    const auto plan = ctx_.select->planPreparedSelect(prepared, *table, rewrite);
    auto rows = ctx_.select->collectRows(prepared, *table, plan);

    const auto columnIndex = table->columnIndex(prepared.columns.front().column);
    if (!columnIndex) {
        // Joined/qualified names are unusual on single-table paths; try result-style resolve.
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

bool SubqueryRuntime::evaluateExists(const Select &subquery) const {
    if (!subquery.setOps.empty()) {
        Select limited = subquery;
        // Prefer a cheap existence check: evaluate fully (set ops need both arms) then test empty.
        auto result = evaluateSelectResult(limited);
        return !result.rows.empty();
    }

    RewriteResult rewrite;
    Select prepared = prepareSelect(subquery, rewrite);
    std::unordered_map<std::string, std::shared_ptr<Table>> temps;
    materializeCteList(rewrite.materialize, temps);
    prepared.limit = 1;
    if (!prepared.joins.empty()) {
        auto result = ctx_.select->executeJoinSelect(prepared, temps);
        if (!result.success) {
            throw std::runtime_error(result.message);
        }
        return !result.rows.empty();
    }
    auto table = ctx_.select->requireTable(prepared.table, temps);
    const auto plan = ctx_.select->planPreparedSelect(prepared, *table, rewrite);
    auto rows = ctx_.select->collectRows(prepared, *table, plan);
    return !rows.empty();
}

bool SubqueryRuntime::matches(const Row &row, const Table &table, const Predicate &predicate,
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
                    const Select bound = bindOuterReferences(*node.subquery, row, table, scope);
                    return evaluateExists(bound);
                }
                return evaluateExists(*node.subquery);
            } else if constexpr (std::is_same_v<T, InSubqueryPred>) {
                if (!node.subquery) {
                    throw std::runtime_error("IN subquery is missing");
                }
                const Select *subquery = node.subquery.get();
                std::optional<Select> bound;
                if (node.referencesOuter || node.subquery->hasOuterRefs) {
                    bound = bindOuterReferences(*node.subquery, row, table, scope);
                    subquery = &*bound;
                }
                const auto values = evaluateSubqueryValues(*subquery);
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

} // namespace VertexDB
