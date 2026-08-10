#include "VertexDB/execution/subquery_runtime.hpp"

#include "VertexDB/common/string_utils.hpp"
#include "VertexDB/execution/recursive_cte_limits.hpp"
#include "VertexDB/execution/select_engine.hpp"
#include "VertexDB/execution/select_helpers.hpp"
#include "VertexDB/execution/select_scope.hpp"
#include "VertexDB/parser/predicate.hpp"

#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace VertexDB {

RecursiveCteLimits &recursiveCteLimits() noexcept {
    static RecursiveCteLimits limits;
    return limits;
}

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
    RewriteResult rewrite;
    const Select prepared = prepareSelect(subquery, rewrite);
    std::unordered_map<std::string, std::shared_ptr<Table>> temps;
    for (const auto &cte : rewrite.materialize) {
        temps.emplace(cte.name, materializeCteTable(cte));
    }

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
    RewriteResult rewrite;
    Select prepared = prepareSelect(subquery, rewrite);
    std::unordered_map<std::string, std::shared_ptr<Table>> temps;
    for (const auto &cte : rewrite.materialize) {
        temps.emplace(cte.name, materializeCteTable(cte));
    }
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

namespace {

[[nodiscard]] std::string_view unqualifiedName(std::string_view name) {
    const auto dot = name.rfind('.');
    if (dot == std::string_view::npos) {
        return name;
    }
    return name.substr(dot + 1);
}

[[nodiscard]] std::optional<std::string_view> qualifier(std::string_view name) {
    const auto dot = name.find('.');
    if (dot == std::string_view::npos) {
        return std::nullopt;
    }
    return name.substr(0, dot);
}

[[nodiscard]] bool refersToCurrentOuter(std::string_view column, const Table &outerTable,
                                        std::string_view outerScope) {
    if (const auto table = qualifier(column)) {
        return equalsIgnoreCase(*table, outerScope) || equalsIgnoreCase(*table, outerTable.name());
    }
    return outerTable.columnIndex(unqualifiedName(column)).has_value();
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

Predicate SubqueryRuntime::bindOuterReferences(const Predicate &predicate, const Row &outerRow,
                                               const Table &outerTable,
                                               std::string_view outerScope) const {
    return std::visit(
        [&](const auto &node) -> Predicate {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, AndPred>) {
                return makeAnd(bindOuterReferences(*node.left, outerRow, outerTable, outerScope),
                               bindOuterReferences(*node.right, outerRow, outerTable, outerScope));
            } else if constexpr (std::is_same_v<T, OrPred>) {
                return makeOr(bindOuterReferences(*node.left, outerRow, outerTable, outerScope),
                              bindOuterReferences(*node.right, outerRow, outerTable, outerScope));
            } else if constexpr (std::is_same_v<T, InSubqueryPred>) {
                InSubqueryPred bound = node;
                if (bound.subquery) {
                    auto sub =
                        bindOuterReferences(*bound.subquery, outerRow, outerTable, outerScope);
                    bound.referencesOuter = sub.hasOuterRefs;
                    bound.subquery = std::make_shared<Select>(std::move(sub));
                } else {
                    bound.referencesOuter = false;
                }
                return bound;
            } else if constexpr (std::is_same_v<T, ExistsPred>) {
                ExistsPred bound = node;
                if (bound.subquery) {
                    auto sub =
                        bindOuterReferences(*bound.subquery, outerRow, outerTable, outerScope);
                    bound.referencesOuter = sub.hasOuterRefs;
                    bound.subquery = std::make_shared<Select>(std::move(sub));
                } else {
                    bound.referencesOuter = false;
                }
                return bound;
            } else if constexpr (std::is_same_v<T, ComparisonPred>) {
                ComparisonPred bound = node;
                if (node.rhsColumn && node.referencesOuter &&
                    refersToCurrentOuter(*node.rhsColumn, outerTable, outerScope)) {
                    bound.rhsColumn.reset();
                    bound.value = outerColumnValue(*node.rhsColumn, outerRow, outerTable);
                    bound.referencesOuter = false;
                } else if (!node.referencesOuter) {
                    bound.referencesOuter = false;
                }
                // Else: still refers to a mid-level outer; leave for a later bind frame.
                return bound;
            } else {
                return node;
            }
        },
        predicate);
}

Select SubqueryRuntime::bindOuterReferences(const Select &subquery, const Row &outerRow,
                                            const Table &outerTable,
                                            std::string_view outerScope) const {
    Select bound = subquery;
    bound.hasOuterRefs = false;
    for (auto &cte : bound.ctes) {
        if (!cte.body) {
            continue;
        }
        auto body = bindOuterReferences(*cte.body, outerRow, outerTable, outerScope);
        if (body.hasOuterRefs) {
            bound.hasOuterRefs = true;
        }
        cte.body = std::make_shared<Select>(std::move(body));
    }
    if (bound.where) {
        bound.where = bindOuterReferences(*bound.where, outerRow, outerTable, outerScope);
        if (predicateReferencesOuter(*bound.where)) {
            bound.hasOuterRefs = true;
        }
    }
    return bound;
}

Select SubqueryRuntime::bindOuterReferences(const Select &subquery, const Row &outerRow,
                                            const Table &outerTable) const {
    return bindOuterReferences(subquery, outerRow, outerTable, outerTable.name());
}

Predicate SubqueryRuntime::bindOuterReferences(const Predicate &predicate, const Row &outerRow,
                                               const Table &outerTable) const {
    return bindOuterReferences(predicate, outerRow, outerTable, outerTable.name());
}

namespace {

[[nodiscard]] std::shared_ptr<Table> tableFromQueryResult(const std::string &name,
                                                          QueryResult bodyResult) {
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
        // Ephemeral CTE tables expose bare column names so later JOINs/filters can reference
        // `id` even when the body projected `Nodes.id`.
        std::string columnName = bodyResult.columns[i];
        if (const auto dot = columnName.rfind('.'); dot != std::string::npos) {
            columnName = columnName.substr(dot + 1);
        }
        schema.push_back({std::move(columnName), type, true});
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

} // namespace

std::shared_ptr<Table> SubqueryRuntime::materializeCteTable(const CteEntry &cte) const {
    if (!cte.body) {
        throw std::runtime_error("materialized CTE is missing a body");
    }

    auto evaluateSelectToResult =
        [&](const Select &body,
            const std::unordered_map<std::string, std::shared_ptr<Table>> &extraTemps =
                {}) -> QueryResult {
        RewriteResult rewrite;
        const Select prepared = prepareSelect(body, rewrite);
        std::unordered_map<std::string, std::shared_ptr<Table>> temps = extraTemps;
        for (const auto &nested : rewrite.materialize) {
            temps.emplace(nested.name, materializeCteTable(nested));
        }
        if (!prepared.joins.empty()) {
            return ctx_.select->executeJoinSelect(prepared, temps);
        }
        auto source = ctx_.select->requireTable(prepared.table, temps);
        const auto plan = ctx_.select->planPreparedSelect(prepared, *source, rewrite);
        std::vector<std::string> sourceColumns;
        for (const auto &column : source->schema()) {
            sourceColumns.push_back(column.name);
        }
        auto rows = ctx_.select->collectRows(prepared, *source, plan);
        return ctx_.select->finalizeSelectResult(prepared, std::move(sourceColumns),
                                                 std::move(rows));
    };

    if (cte.recursive) {
        if (!cte.recursiveArm) {
            throw std::runtime_error("recursive CTE is missing a recursive arm");
        }
        auto anchorResult = evaluateSelectToResult(*cte.body);
        // Copy rows before first tableFromQueryResult moves them out of anchorResult.
        QueryResult deltaSource = anchorResult;
        auto result = tableFromQueryResult(cte.name, std::move(anchorResult));
        auto delta = tableFromQueryResult(cte.name + "#delta", std::move(deltaSource));

        for (std::size_t iter = 0; iter < recursiveCteLimits().maxIterations; ++iter) {
            if (delta->rowCount() == 0) {
                break;
            }
            std::unordered_map<std::string, std::shared_ptr<Table>> temps;
            temps.emplace(cte.name, delta);
            auto stepResult = evaluateSelectToResult(*cte.recursiveArm, temps);
            if (result->rowCount() + stepResult.rows.size() > recursiveCteLimits().maxRows) {
                throw std::runtime_error("WITH RECURSIVE exceeded maximum row count");
            }
            QueryResult deltaCopy = stepResult;
            for (const auto &row : stepResult.rows) {
                result->insert(row);
            }
            delta = tableFromQueryResult(cte.name + "#delta", std::move(deltaCopy));
            if (iter + 1 == recursiveCteLimits().maxIterations && delta->rowCount() > 0) {
                throw std::runtime_error("WITH RECURSIVE exceeded maximum iteration count");
            }
        }
        return result;
    }

    return tableFromQueryResult(cte.name, evaluateSelectToResult(*cte.body));
}

} // namespace VertexDB
