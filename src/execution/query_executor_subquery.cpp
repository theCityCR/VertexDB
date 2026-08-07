#include "VertexDB/execution/query_executor.hpp"

#include "VertexDB/execution/select_helpers.hpp"
#include "VertexDB/planner/rewriter.hpp"

#include <memory>
#include <stdexcept>
#include <utility>

namespace VertexDB {

Select QueryExecutor::prepareSelect(const Select &command, RewriteResult &rewrite) const {
    rewrite = rewriteSelect(command);
    Select prepared = rewrite.query;
    if (prepared.where) {
        prepared.where = materializePredicate(*prepared.where);
    }
    return prepared;
}

Predicate QueryExecutor::materializePredicate(const Predicate &predicate) const {
    if (predicate.kind == Predicate::Kind::And || predicate.kind == Predicate::Kind::Or) {
        return Predicate{predicate.kind, std::make_shared<Predicate>(materializePredicate(*predicate.left)),
                         std::make_shared<Predicate>(materializePredicate(*predicate.right))};
    }
    if (predicate.kind == Predicate::Kind::Exists) {
        return predicate;
    }
    if (predicate.kind == Predicate::Kind::InSubquery) {
        if (!predicate.subquery) {
            throw std::runtime_error("IN subquery is missing");
        }
        if (predicate.referencesOuter || predicate.subquery->hasOuterRefs) {
            return predicate;
        }
        return Predicate{predicate.column, evaluateSubqueryValues(*predicate.subquery)};
    }
    return predicate;
}

std::vector<Value> QueryExecutor::evaluateSubqueryValues(const Select &subquery) const {
    RewriteResult rewrite;
    const Select prepared = prepareSelect(subquery, rewrite);
    if (!prepared.joins.empty()) {
        throw std::runtime_error("JOIN inside IN subquery is not supported");
    }
    auto table = requireTable(prepared.table);
    const auto plan = planPreparedSelect(prepared, *table, rewrite);
    auto rows = collectRows(prepared, *table, plan);

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

bool QueryExecutor::evaluateExists(const Select &subquery) const {
    RewriteResult rewrite;
    Select prepared = prepareSelect(subquery, rewrite);
    if (!prepared.joins.empty()) {
        throw std::runtime_error("JOIN inside EXISTS subquery is not supported");
    }
    prepared.limit = 1;
    auto table = requireTable(prepared.table);
    const auto plan = planPreparedSelect(prepared, *table, rewrite);
    auto rows = collectRows(prepared, *table, plan);
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

Predicate QueryExecutor::bindOuterReferences(const Predicate &predicate, const Row &outerRow,
                                             const Table &outerTable) const {
    if (predicate.kind == Predicate::Kind::And || predicate.kind == Predicate::Kind::Or) {
        return Predicate{predicate.kind,
                         std::make_shared<Predicate>(
                             bindOuterReferences(*predicate.left, outerRow, outerTable)),
                         std::make_shared<Predicate>(
                             bindOuterReferences(*predicate.right, outerRow, outerTable))};
    }
    if (predicate.kind == Predicate::Kind::InSubquery || predicate.kind == Predicate::Kind::Exists) {
        throw std::runtime_error("multi-level correlated subqueries are not supported");
    }
    if (predicate.kind != Predicate::Kind::Comparison) {
        return predicate;
    }
    Predicate bound = predicate;
    bound.referencesOuter = false;
    if (predicate.rhsColumn) {
        bound.rhsColumn.reset();
        bound.value = outerColumnValue(*predicate.rhsColumn, outerRow, outerTable);
    }
    return bound;
}

Select QueryExecutor::bindOuterReferences(const Select &subquery, const Row &outerRow,
                                          const Table &outerTable) const {
    Select bound = subquery;
    bound.hasOuterRefs = false;
    if (bound.where) {
        bound.where = bindOuterReferences(*bound.where, outerRow, outerTable);
    }
    return bound;
}

std::shared_ptr<Table> QueryExecutor::materializeCteTable(const std::string &name,
                                                          const Select &body) const {
    RewriteResult rewrite;
    const Select prepared = prepareSelect(body, rewrite);
    QueryResult bodyResult;
    if (!prepared.joins.empty()) {
        bodyResult = const_cast<QueryExecutor *>(this)->executeJoinSelect(prepared);
    } else {
        auto source = requireTable(prepared.table);
        const auto plan = planPreparedSelect(prepared, *source, rewrite);
        std::vector<std::string> sourceColumns;
        for (const auto &column : source->schema()) {
            sourceColumns.push_back(column.name);
        }
        auto rows = collectRows(prepared, *source, plan);
        bodyResult = finalizeSelectResult(prepared, std::move(sourceColumns), std::move(rows));
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
            const auto projection = resolveProjection(prepared, *source, projectedNames);
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
