#include "VertexDB/execution/subquery_runtime.hpp"

#include "VertexDB/execution/query_executor.hpp"
#include "VertexDB/execution/select_helpers.hpp"

#include <memory>
#include <stdexcept>
#include <utility>

namespace VertexDB {

SubqueryRuntime::SubqueryRuntime(QueryExecutor &owner) noexcept : owner_(owner) {}

Select SubqueryRuntime::prepareSelect(const Select &command, RewriteResult &rewrite) const {
    rewrite = rewriteSelect(command);
    Select prepared = rewrite.query;
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
    if (!prepared.joins.empty()) {
        throw std::runtime_error("JOIN inside IN subquery is not supported");
    }
    auto table = owner_.selectEngine_.requireTable(prepared.table);
    const auto plan = owner_.selectEngine_.planPreparedSelect(prepared, *table, rewrite);
    auto rows = owner_.selectEngine_.collectRows(prepared, *table, plan);

    if (prepared.columns.size() != 1 || isStarProjection(prepared.columns) ||
        prepared.columns.front().kind != SelectExpr::Kind::Column) {
        throw std::runtime_error("IN subquery must project exactly one column");
    }
    const auto columnIndex = table->columnIndex(prepared.columns.front().column);
    if (!columnIndex) {
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
    if (!prepared.joins.empty()) {
        throw std::runtime_error("JOIN inside EXISTS subquery is not supported");
    }
    prepared.limit = 1;
    auto table = owner_.selectEngine_.requireTable(prepared.table);
    const auto plan = owner_.selectEngine_.planPreparedSelect(prepared, *table, rewrite);
    auto rows = owner_.selectEngine_.collectRows(prepared, *table, plan);
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
                                               const Table &outerTable) const {
    return std::visit(
        [&](const auto &node) -> Predicate {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, AndPred>) {
                return makeAnd(bindOuterReferences(*node.left, outerRow, outerTable),
                               bindOuterReferences(*node.right, outerRow, outerTable));
            } else if constexpr (std::is_same_v<T, OrPred>) {
                return makeOr(bindOuterReferences(*node.left, outerRow, outerTable),
                              bindOuterReferences(*node.right, outerRow, outerTable));
            } else if constexpr (std::is_same_v<T, InSubqueryPred> ||
                                 std::is_same_v<T, ExistsPred>) {
                throw std::runtime_error("multi-level correlated subqueries are not supported");
            } else if constexpr (std::is_same_v<T, ComparisonPred>) {
                ComparisonPred bound = node;
                bound.referencesOuter = false;
                if (node.rhsColumn) {
                    bound.rhsColumn.reset();
                    bound.value = outerColumnValue(*node.rhsColumn, outerRow, outerTable);
                }
                return bound;
            } else {
                return node;
            }
        },
        predicate);
}

Select SubqueryRuntime::bindOuterReferences(const Select &subquery, const Row &outerRow,
                                            const Table &outerTable) const {
    Select bound = subquery;
    bound.hasOuterRefs = false;
    if (bound.where) {
        bound.where = bindOuterReferences(*bound.where, outerRow, outerTable);
    }
    return bound;
}

std::shared_ptr<Table> SubqueryRuntime::materializeCteTable(const std::string &name,
                                                           const Select &body) const {
    RewriteResult rewrite;
    const Select prepared = prepareSelect(body, rewrite);
    QueryResult bodyResult;
    if (!prepared.joins.empty()) {
        bodyResult = owner_.selectEngine_.executeJoinSelect(prepared);
    } else {
        auto source = owner_.selectEngine_.requireTable(prepared.table);
        const auto plan = owner_.selectEngine_.planPreparedSelect(prepared, *source, rewrite);
        std::vector<std::string> sourceColumns;
        for (const auto &column : source->schema()) {
            sourceColumns.push_back(column.name);
        }
        auto rows = owner_.selectEngine_.collectRows(prepared, *source, plan);
        bodyResult = owner_.selectEngine_.finalizeSelectResult(
            prepared, std::move(sourceColumns), std::move(rows));
        if (!bodyResult.success) {
            throw std::runtime_error(bodyResult.message);
        }
        std::vector<Column> schema;
        schema.reserve(bodyResult.columns.size());
        if (hasAggregates(prepared.columns) || !prepared.groupBy.empty()) {
            for (std::size_t i = 0; i < bodyResult.columns.size(); ++i) {
                ColumnType type = ColumnType::Int;
                for (const auto &row : bodyResult.rows) {
                    if (!row[i].isNull()) {
                        type = row[i].type();
                        break;
                    }
                }
                schema.push_back({bodyResult.columns[i], type, true});
            }
        } else {
            std::vector<std::string> projectedNames;
            const auto projection =
                owner_.selectEngine_.resolveProjection(prepared, *source, projectedNames);
            schema.reserve(projection.size());
            for (std::size_t i = 0; i < projection.size(); ++i) {
                const auto &sourceColumn = source->schema()[projection[i]];
                schema.push_back({bodyResult.columns[i], sourceColumn.type, sourceColumn.nullable});
            }
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
        schema.push_back({bodyResult.columns[i], type, true});
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

} // namespace VertexDB
