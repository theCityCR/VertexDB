#include "VertexDB/execution/subquery_runtime.hpp"

#include "VertexDB/common/string_utils.hpp"
#include "VertexDB/execution/recursive_cte_limits.hpp"
#include "VertexDB/execution/select_engine.hpp"
#include "VertexDB/execution/select_helpers.hpp"
#include "VertexDB/execution/set_ops.hpp"
#include "VertexDB/parser/recursive_cte.hpp"
#include "VertexDB/planner/rewriter.hpp"

#include <functional>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace VertexDB {

RecursiveCteLimits &recursiveCteLimits() noexcept {
    static RecursiveCteLimits limits;
    return limits;
}

namespace {

[[nodiscard]] bool tempsContains(const std::unordered_map<std::string, std::shared_ptr<Table>> &temps,
                                 std::string_view name) {
    for (const auto &[key, value] : temps) {
        (void)value;
        if (equalsIgnoreCase(key, name)) {
            return true;
        }
    }
    return false;
}

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

struct RecursiveWorking {
    CteEntry cte;
    std::shared_ptr<Table> result;
    std::shared_ptr<Table> delta;
    std::unordered_set<Row, RowHash> seen;
};

[[nodiscard]] std::unordered_map<std::string, std::shared_ptr<Table>> materializeRecursiveGroup(
    const std::vector<CteEntry> &group,
    const std::unordered_map<std::string, std::shared_ptr<Table>> &extraTemps,
    const std::function<QueryResult(const Select &,
                                    const std::unordered_map<std::string, std::shared_ptr<Table>> &)>
        &evaluate) {
    if (group.empty()) {
        throw std::runtime_error("recursive CTE group is empty");
    }

    std::vector<RecursiveWorking> workings;
    workings.reserve(group.size());
    for (const auto &cte : group) {
        if (!cte.body || !cte.recursiveArm) {
            throw std::runtime_error("recursive CTE is missing a recursive arm");
        }
        auto anchorResult = evaluate(*cte.body, extraTemps);
        if (cte.recursiveDistinct) {
            anchorResult.rows = deduplicateRows(std::move(anchorResult.rows));
        }
        RecursiveWorking working;
        working.cte = cte;
        if (cte.recursiveDistinct) {
            for (const auto &row : anchorResult.rows) {
                working.seen.insert(row);
            }
        }
        QueryResult deltaSource = anchorResult;
        working.result = tableFromQueryResult(cte.name, std::move(anchorResult));
        working.delta = tableFromQueryResult(cte.name + "#delta", std::move(deltaSource));
        workings.push_back(std::move(working));
    }

    for (std::size_t iter = 0; iter < recursiveCteLimits().maxIterations; ++iter) {
        bool anyDelta = false;
        for (const auto &working : workings) {
            if (working.delta->rowCount() > 0) {
                anyDelta = true;
                break;
            }
        }
        if (!anyDelta) {
            break;
        }

        std::unordered_map<std::string, std::shared_ptr<Table>> temps = extraTemps;
        for (const auto &working : workings) {
            temps[working.cte.name] =
                working.cte.recursiveBindAccumulator ? working.result : working.delta;
        }

        std::vector<QueryResult> steps;
        steps.reserve(workings.size());
        for (const auto &working : workings) {
            steps.push_back(evaluate(*working.cte.recursiveArm, temps));
        }

        for (std::size_t i = 0; i < workings.size(); ++i) {
            if (!workings[i].cte.recursiveDistinct) {
                continue;
            }
            QueryResult filtered = steps[i];
            filtered.rows.clear();
            for (auto &row : steps[i].rows) {
                if (workings[i].seen.insert(row).second) {
                    filtered.rows.push_back(std::move(row));
                }
            }
            steps[i] = std::move(filtered);
        }

        std::size_t totalRows = 0;
        std::size_t stepRows = 0;
        for (const auto &working : workings) {
            totalRows += working.result->rowCount();
        }
        for (const auto &step : steps) {
            stepRows += step.rows.size();
        }
        if (totalRows + stepRows > recursiveCteLimits().maxRows) {
            throw std::runtime_error("WITH RECURSIVE exceeded maximum row count");
        }

        for (std::size_t i = 0; i < workings.size(); ++i) {
            QueryResult deltaCopy = steps[i];
            // Keep working-table column names so later joins can still reference `id`
            // even when the arm projected `Edge.next_id`.
            deltaCopy.columns.clear();
            for (const auto &column : workings[i].result->schema()) {
                deltaCopy.columns.push_back(column.name);
            }
            for (const auto &row : steps[i].rows) {
                workings[i].result->insert(row);
            }
            workings[i].delta =
                tableFromQueryResult(workings[i].cte.name + "#delta", std::move(deltaCopy));
        }
        bool stillGrowing = false;
        for (const auto &working : workings) {
            if (working.delta->rowCount() > 0) {
                stillGrowing = true;
                break;
            }
        }
        if (iter + 1 == recursiveCteLimits().maxIterations && stillGrowing) {
            throw std::runtime_error("WITH RECURSIVE exceeded maximum iteration count");
        }
    }

    std::unordered_map<std::string, std::shared_ptr<Table>> out;
    for (auto &working : workings) {
        out.emplace(std::move(working.cte.name), std::move(working.result));
    }
    return out;
}

} // namespace

QueryResult SubqueryRuntime::evaluateSelectResult(
    const Select &body,
    const std::unordered_map<std::string, std::shared_ptr<Table>> &extraTemps) const {
    if (!body.setOps.empty()) {
        Select left = body;
        left.setOps.clear();
        left.orderBy = std::nullopt;
        left.limit = std::nullopt;
        QueryResult combined = evaluateSelectResult(left, extraTemps);
        for (const auto &arm : body.setOps) {
            if (!arm.select) {
                throw std::runtime_error("set operation is missing a SELECT arm");
            }
            Select right = *arm.select;
            if (right.ctes.empty() && !body.ctes.empty()) {
                right.ctes = body.ctes;
            }
            combined = applySetOperation(arm.op, std::move(combined),
                                         evaluateSelectResult(right, extraTemps));
        }
        if (body.orderBy) {
            const auto orderColumn = resolveResultColumn(combined.columns, body.orderBy->column);
            if (!orderColumn) {
                throw std::runtime_error("unknown ORDER BY column");
            }
            sortRowsByColumn(combined.rows, *orderColumn, body.orderBy->ascending);
        }
        if (body.limit && combined.rows.size() > *body.limit) {
            combined.rows.resize(*body.limit);
        }
        return combined;
    }

    RewriteResult rewrite;
    const Select prepared = prepareSelect(body, rewrite);
    std::unordered_map<std::string, std::shared_ptr<Table>> temps = extraTemps;
    materializeCteList(rewrite.materialize, temps);
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
    return ctx_.select->finalizeSelectResult(prepared, std::move(sourceColumns), std::move(rows));
}

void SubqueryRuntime::materializeCteList(
    const std::vector<CteEntry> &ctes,
    std::unordered_map<std::string, std::shared_ptr<Table>> &temps) const {
    for (const auto &cte : ctes) {
        if (tempsContains(temps, cte.name)) {
            continue;
        }
        if (cte.recursive) {
            auto group = recursiveSccContaining(ctes, cte.name);
            if (group.empty()) {
                group.push_back(cte);
            }
            // Skip members already present (partial overlap with prior work).
            std::vector<CteEntry> pending;
            for (const auto &member : group) {
                if (!tempsContains(temps, member.name)) {
                    pending.push_back(member);
                }
            }
            if (pending.empty()) {
                continue;
            }
            auto evaluate = [this](const Select &select,
                                   const std::unordered_map<std::string, std::shared_ptr<Table>>
                                       &extra) { return evaluateSelectResult(select, extra); };
            auto tables = materializeRecursiveGroup(pending, temps, evaluate);
            for (auto &[name, table] : tables) {
                temps.emplace(std::move(name), std::move(table));
            }
            continue;
        }
        if (!cte.body) {
            throw std::runtime_error("materialized CTE is missing a body");
        }
        temps.emplace(cte.name, tableFromQueryResult(cte.name, evaluateSelectResult(*cte.body, temps)));
    }
}

std::shared_ptr<Table> SubqueryRuntime::materializeCteTable(
    const CteEntry &cte,
    const std::unordered_map<std::string, std::shared_ptr<Table>> &extraTemps) const {
    std::unordered_map<std::string, std::shared_ptr<Table>> temps = extraTemps;
    materializeCteList({cte}, temps);
    for (const auto &[name, table] : temps) {
        if (equalsIgnoreCase(name, cte.name)) {
            return table;
        }
    }
    throw std::runtime_error("failed to materialize CTE " + cte.name);
}

} // namespace VertexDB
