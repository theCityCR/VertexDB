#include "VertexDB/execution/subquery_runtime.hpp"

#include "VertexDB/execution/recursive_cte_limits.hpp"
#include "VertexDB/execution/select_engine.hpp"
#include "VertexDB/execution/select_helpers.hpp"
#include "VertexDB/planner/rewriter.hpp"

#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace VertexDB {

RecursiveCteLimits &recursiveCteLimits() noexcept {
    static RecursiveCteLimits limits;
    return limits;
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
