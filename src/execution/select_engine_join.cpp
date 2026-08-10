#include "VertexDB/execution/select_engine.hpp"

#include "VertexDB/common/comparison_operator.hpp"
#include "VertexDB/execution/predicate_eval.hpp"
#include "VertexDB/execution/select_helpers.hpp"
#include "VertexDB/planner/query_planner.hpp"

#include <map>
#include <stdexcept>
#include <utility>

namespace VertexDB {

void SelectEngine::collectJoinRows(
    const Select &command, std::vector<std::string> &joinedColumns, std::vector<Row> &joinedRows,
    const std::unordered_map<std::string, std::shared_ptr<Table>> &temps,
    ExplainAnalyzeStats *stats) const {
    if (command.joins.empty()) {
        throw std::runtime_error("collectJoinRows requires at least one join");
    }

    auto leftTable = requireTable(command.table, temps);
    joinedColumns.clear();
    for (const auto &column : leftTable->schema()) {
        joinedColumns.push_back(command.table + "." + column.name);
    }
    joinedRows = rowsSnapshotForRead(*leftTable);

    for (std::size_t joinIndex = 0; joinIndex < command.joins.size(); ++joinIndex) {
        const auto &join = command.joins[joinIndex];
        auto rightTable = requireTable(join.table, temps);

        JoinPlan joinPlan;
        if (joinIndex == 0) {
            joinPlan = ctx_.planner.planJoin(*leftTable, *rightTable, join);
        } else {
            joinPlan = ctx_.planner.planJoinAgainstRows(joinedRows.size(), *rightTable, join);
        }

        std::vector<std::string> nextColumns = joinedColumns;
        for (const auto &column : rightTable->schema()) {
            nextColumns.push_back(join.table + "." + column.name);
        }

        std::vector<Row> nextRows;
        auto appendJoined = [&](const Row &leftRow, const Row &rightRow) {
            Row joined;
            joined.reserve(leftRow.size() + rightRow.size());
            joined.insert(joined.end(), leftRow.begin(), leftRow.end());
            joined.insert(joined.end(), rightRow.begin(), rightRow.end());
            nextRows.push_back(std::move(joined));
        };
        auto appendLeftOnly = [&](const Row &leftRow) {
            Row joined = leftRow;
            joined.resize(leftRow.size() + rightTable->schema().size(), Value{});
            nextRows.push_back(std::move(joined));
        };
        auto appendRightOnly = [&](const Row &rightRow) {
            Row joined(joinedColumns.size(), Value{});
            joined.insert(joined.end(), rightRow.begin(), rightRow.end());
            nextRows.push_back(std::move(joined));
        };

        auto unqualified = [](const std::string &name) {
            const auto dot = name.rfind('.');
            return dot == std::string::npos ? name : name.substr(dot + 1);
        };

        const bool padLeft = join.kind == JoinKind::LeftOuter || join.kind == JoinKind::FullOuter;
        const bool padRight = join.kind == JoinKind::RightOuter || join.kind == JoinKind::FullOuter;

        if (join.kind == JoinKind::Cross) {
            const auto rightRows = rowsSnapshotForRead(*rightTable);
            for (const auto &leftRow : joinedRows) {
                for (const auto &rightRow : rightRows) {
                    appendJoined(leftRow, rightRow);
                }
            }
        } else {
            const auto leftJoinColumn = resolveResultColumn(joinedColumns, join.leftColumn);
            const std::optional<std::string_view> rightAlias =
                join.tableAlias ? std::optional<std::string_view>{*join.tableAlias} : std::nullopt;
            const auto rightJoinColumn =
                resolveTableColumn(*rightTable, join.table, join.rightColumn, rightAlias);
            if (!leftJoinColumn || !rightJoinColumn) {
                throw std::runtime_error("unknown join column");
            }

            const bool equi = join.op == ComparisonOperator::Equal;

            if (equi && joinPlan.algorithm == JoinAlgorithm::NestedLoopIndexProbe &&
                joinPlan.outerIsLeft && !joinPlan.probeColumn.empty() && !padRight) {
                const auto probeColumn = unqualified(join.rightColumn);
                for (const auto &leftRow : joinedRows) {
                    bool matched = false;
                    if (auto rowIds =
                            rightTable->indexedLookup(probeColumn, leftRow[*leftJoinColumn])) {
                        for (const auto &rightRow : rowsByIdForRead(*rightTable, *rowIds)) {
                            appendJoined(leftRow, rightRow);
                            matched = true;
                        }
                    }
                    if (padLeft && !matched) {
                        appendLeftOnly(leftRow);
                    }
                }
            } else if (equi && joinPlan.algorithm == JoinAlgorithm::NestedLoopIndexProbe &&
                       !joinPlan.outerIsLeft && joinIndex == 0 && !padLeft) {
                // RIGHT (or INNER with right-as-outer): scan right, probe left indexes.
                const auto outerRows = rowsSnapshotForRead(*rightTable);
                const auto probeColumn = unqualified(join.leftColumn);
                for (const auto &rightRow : outerRows) {
                    bool matched = false;
                    if (auto rowIds =
                            leftTable->indexedLookup(probeColumn, rightRow[*rightJoinColumn])) {
                        for (const auto &leftRow : rowsByIdForRead(*leftTable, *rowIds)) {
                            appendJoined(leftRow, rightRow);
                            matched = true;
                        }
                    }
                    if (padRight && !matched) {
                        appendRightOnly(rightRow);
                    }
                }
            } else if (equi && joinPlan.algorithm == JoinAlgorithm::HashJoin && !padRight) {
                const auto rightRows = rowsSnapshotForRead(*rightTable);
                std::map<Value, std::vector<Row>> rightRowsByKey;
                for (const auto &row : rightRows) {
                    rightRowsByKey[row[*rightJoinColumn]].push_back(row);
                }
                for (const auto &leftRow : joinedRows) {
                    auto matchingRightRows = rightRowsByKey.find(leftRow[*leftJoinColumn]);
                    if (matchingRightRows == rightRowsByKey.end()) {
                        if (padLeft) {
                            appendLeftOnly(leftRow);
                        }
                        continue;
                    }
                    for (const auto &rightRow : matchingRightRows->second) {
                        appendJoined(leftRow, rightRow);
                    }
                }
            } else {
                // Nested-loop compare with optional unmatched padding on either side.
                const auto rightRows = rowsSnapshotForRead(*rightTable);
                std::vector<char> rightMatched(rightRows.size(), 0);
                for (const auto &leftRow : joinedRows) {
                    bool matched = false;
                    for (std::size_t ri = 0; ri < rightRows.size(); ++ri) {
                        if (compareValues(leftRow[*leftJoinColumn], join.op,
                                          rightRows[ri][*rightJoinColumn])) {
                            appendJoined(leftRow, rightRows[ri]);
                            matched = true;
                            rightMatched[ri] = 1;
                        }
                    }
                    if (padLeft && !matched) {
                        appendLeftOnly(leftRow);
                    }
                }
                if (padRight) {
                    for (std::size_t ri = 0; ri < rightRows.size(); ++ri) {
                        if (!rightMatched[ri]) {
                            appendRightOnly(rightRows[ri]);
                        }
                    }
                }
            }
        }

        joinedColumns = std::move(nextColumns);
        joinedRows = std::move(nextRows);
        if (stats) {
            stats->joinActualRows.push_back(joinedRows.size());
        }
    }

    if (command.where) {
        const ColumnLookup joinLookup = [&](std::string_view column) {
            return resolveResultColumn(joinedColumns, column);
        };
        std::vector<Row> filtered;
        filtered.reserve(joinedRows.size());
        for (auto &row : joinedRows) {
            if (evalPredicate(*command.where, row, joinLookup)) {
                filtered.push_back(std::move(row));
            }
        }
        joinedRows = std::move(filtered);
        if (stats && !stats->joinActualRows.empty()) {
            stats->joinActualRows.back() = joinedRows.size();
        }
    }
    if (stats) {
        stats->actualRows = joinedRows.size();
    }
}

QueryResult SelectEngine::executeJoinSelect(
    const Select &command,
    const std::unordered_map<std::string, std::shared_ptr<Table>> &temps) {
    std::vector<std::string> joinedColumns;
    std::vector<Row> joinedRows;
    collectJoinRows(command, joinedColumns, joinedRows, temps);
    return finalizeSelectResult(command, std::move(joinedColumns), std::move(joinedRows));
}

} // namespace VertexDB
