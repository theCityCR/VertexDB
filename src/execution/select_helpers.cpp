#include "VertexDB/execution/select_helpers.hpp"

#include <algorithm>
#include <map>
#include <memory>
#include <stdexcept>
#include <variant>

namespace VertexDB {
namespace {

[[nodiscard]] Value bindValue(const Value &value, const std::vector<Value> &parameters,
                              std::vector<bool> &seen) {
    if (!value.isParameter()) {
        return value;
    }
    const auto index = value.parameterIndex();
    if (index >= parameters.size()) {
        throw std::runtime_error("not enough prepared statement parameters");
    }
    seen[index] = true;
    return parameters[index];
}

[[nodiscard]] IndexExpression bindIndexExpression(IndexExpression expression,
                                                  const std::vector<Value> &parameters,
                                                  std::vector<bool> &seen) {
    expression.literal = bindValue(expression.literal, parameters, seen);
    return expression;
}

Predicate bindPredicate(const Predicate &predicate, const std::vector<Value> &parameters,
                        std::vector<bool> &seen);

Select bindSelect(const Select &select, const std::vector<Value> &parameters, std::vector<bool> &seen);

Predicate bindPredicate(const Predicate &predicate, const std::vector<Value> &parameters,
                        std::vector<bool> &seen) {
    Predicate bound = predicate;
    if (predicate.kind == Predicate::Kind::And || predicate.kind == Predicate::Kind::Or) {
        bound.left = std::make_shared<Predicate>(bindPredicate(*predicate.left, parameters, seen));
        bound.right = std::make_shared<Predicate>(bindPredicate(*predicate.right, parameters, seen));
        return bound;
    }
    if (predicate.kind == Predicate::Kind::InList) {
        for (auto &value : bound.inValues) {
            value = bindValue(value, parameters, seen);
        }
        return bound;
    }
    if (predicate.kind == Predicate::Kind::InSubquery || predicate.kind == Predicate::Kind::Exists) {
        if (predicate.subquery) {
            bound.subquery = std::make_shared<Select>(bindSelect(*predicate.subquery, parameters, seen));
        }
        return bound;
    }
    bound.value = bindValue(predicate.value, parameters, seen);
    if (predicate.expression) {
        bound.expression = bindIndexExpression(*predicate.expression, parameters, seen);
    }
    return bound;
}

Select bindSelect(const Select &select, const std::vector<Value> &parameters, std::vector<bool> &seen) {
    Select bound = select;
    if (bound.where) {
        bound.where = bindPredicate(*bound.where, parameters, seen);
    }
    for (auto &cte : bound.ctes) {
        if (cte.body) {
            cte.body = std::make_shared<Select>(bindSelect(*cte.body, parameters, seen));
        }
    }
    return bound;
}

struct AggregateAccumulator {
    AggregateOp op{AggregateOp::CountStar};
    std::optional<std::size_t> argIndex;
    std::int64_t count{0};
    bool hasNumeric{false};
    bool isDouble{false};
    std::int64_t intSum{0};
    double doubleSum{0.0};
    Value minValue;
    Value maxValue;
    bool hasMinMax{false};

    void observe(const Row &row) {
        if (op == AggregateOp::CountStar) {
            ++count;
            return;
        }
        const Value &value = row[*argIndex];
        if (value.isNull()) {
            return;
        }
        switch (op) {
        case AggregateOp::CountStar:
            break;
        case AggregateOp::Count:
            ++count;
            break;
        case AggregateOp::Sum:
        case AggregateOp::Avg:
            ++count;
            if (value.type() == ColumnType::Double) {
                if (!hasNumeric) {
                    isDouble = true;
                    doubleSum = std::get<double>(value.data());
                    hasNumeric = true;
                } else if (isDouble) {
                    doubleSum += std::get<double>(value.data());
                } else {
                    isDouble = true;
                    doubleSum = static_cast<double>(intSum) + std::get<double>(value.data());
                }
            } else if (value.type() == ColumnType::Int) {
                if (!hasNumeric) {
                    intSum = std::get<std::int64_t>(value.data());
                    hasNumeric = true;
                } else if (isDouble) {
                    doubleSum += static_cast<double>(std::get<std::int64_t>(value.data()));
                } else {
                    intSum += std::get<std::int64_t>(value.data());
                }
            } else {
                throw std::runtime_error("SUM/AVG require numeric arguments");
            }
            break;
        case AggregateOp::Min:
            if (!hasMinMax || value < minValue) {
                minValue = value;
                hasMinMax = true;
            }
            break;
        case AggregateOp::Max:
            if (!hasMinMax || maxValue < value) {
                maxValue = value;
                hasMinMax = true;
            }
            break;
        }
    }

    [[nodiscard]] Value result() const {
        switch (op) {
        case AggregateOp::CountStar:
        case AggregateOp::Count:
            return Value{count};
        case AggregateOp::Sum:
            if (!hasNumeric) {
                return Value{};
            }
            return isDouble ? Value{doubleSum} : Value{intSum};
        case AggregateOp::Avg:
            if (count == 0) {
                return Value{};
            }
            if (isDouble) {
                return Value{doubleSum / static_cast<double>(count)};
            }
            return Value{static_cast<double>(intSum) / static_cast<double>(count)};
        case AggregateOp::Min:
            return hasMinMax ? minValue : Value{};
        case AggregateOp::Max:
            return hasMinMax ? maxValue : Value{};
        }
        return Value{};
    }
};

[[nodiscard]] bool groupByContains(const std::vector<std::string> &groupBy, std::string_view column) {
    for (const auto &entry : groupBy) {
        if (entry == column) {
            return true;
        }
        const auto suffix = std::string{"."} + std::string{column};
        if (entry.size() >= suffix.size() &&
            entry.compare(entry.size() - suffix.size(), suffix.size(), suffix) == 0) {
            return true;
        }
        if (column.size() > entry.size() && column.ends_with(std::string{"."} + entry)) {
            return true;
        }
    }
    return false;
}

} // namespace

void sortRowsByColumn(std::vector<Row> &rows, std::size_t columnIndex, bool ascending) {
    std::ranges::sort(rows, [&](const Row &left, const Row &right) {
        if (ascending) {
            return left[columnIndex] < right[columnIndex];
        }
        return right[columnIndex] < left[columnIndex];
    });
}

QueryResult projectWithLimit(std::vector<Row> rows, const std::vector<std::size_t> &projection,
                             std::vector<std::string> columns, std::optional<std::size_t> limit,
                             std::string message) {
    QueryResult result{true, std::move(message), std::move(columns), {}};
    for (const auto &row : rows) {
        Row projected;
        projected.reserve(projection.size());
        for (const auto index : projection) {
            projected.push_back(row[index]);
        }
        result.rows.push_back(std::move(projected));
        if (limit && result.rows.size() >= *limit) {
            break;
        }
    }
    return result;
}

std::optional<std::size_t> resolveResultColumn(std::span<const std::string> columns,
                                               std::string_view requested) {
    for (std::size_t i = 0; i < columns.size(); ++i) {
        if (columns[i] == requested) {
            return i;
        }
    }

    std::optional<std::size_t> match;
    const auto suffix = "." + std::string{requested};
    for (std::size_t i = 0; i < columns.size(); ++i) {
        if (columns[i].size() < suffix.size()) {
            continue;
        }
        if (columns[i].compare(columns[i].size() - suffix.size(), suffix.size(), suffix) == 0) {
            if (match) {
                throw std::runtime_error("ambiguous column reference");
            }
            match = i;
        }
    }
    return match;
}

std::optional<std::size_t> resolveTableColumn(const Table &table, std::string_view tableName,
                                              std::string_view requested) {
    const auto qualifier = std::string{tableName} + ".";
    if (requested.starts_with(qualifier)) {
        requested.remove_prefix(qualifier.size());
    }
    return table.columnIndex(requested);
}

QueryResult messageResult(bool success, std::string message) {
    QueryResult result;
    result.success = success;
    result.message = std::move(message);
    return result;
}

void validateAggregation(const Select &command) {
    const bool aggregating = hasAggregates(command.columns) || !command.groupBy.empty();
    if (!aggregating) {
        return;
    }
    if (isStarProjection(command.columns)) {
        throw std::runtime_error("SELECT * is not allowed with aggregates or GROUP BY");
    }
    for (const auto &expr : command.columns) {
        if (expr.kind == SelectExpr::Kind::Star) {
            throw std::runtime_error("SELECT * is not allowed with aggregates or GROUP BY");
        }
        if (expr.kind == SelectExpr::Kind::Column &&
            !groupByContains(command.groupBy, expr.column)) {
            throw std::runtime_error("selected column must appear in GROUP BY or an aggregate");
        }
        if (expr.kind == SelectExpr::Kind::Aggregate && expr.aggregate != AggregateOp::CountStar &&
            !expr.aggregateArg) {
            throw std::runtime_error("aggregate is missing a column argument");
        }
    }
}

QueryResult aggregateRows(const Select &command, const std::vector<std::string> &sourceColumns,
                          std::vector<Row> rows) {
    validateAggregation(command);

    std::vector<std::size_t> groupIndices;
    groupIndices.reserve(command.groupBy.size());
    for (const auto &column : command.groupBy) {
        auto index = resolveResultColumn(sourceColumns, column);
        if (!index) {
            throw std::runtime_error("unknown GROUP BY column");
        }
        groupIndices.push_back(*index);
    }

    struct GroupState {
        Row key;
        std::vector<AggregateAccumulator> aggregates;
    };

    std::map<Row, GroupState> groups;
    auto makeAccumulators = [&]() {
        std::vector<AggregateAccumulator> acc;
        for (const auto &expr : command.columns) {
            if (expr.kind != SelectExpr::Kind::Aggregate) {
                continue;
            }
            AggregateAccumulator state;
            state.op = expr.aggregate;
            if (expr.aggregate != AggregateOp::CountStar) {
                auto index = resolveResultColumn(sourceColumns, *expr.aggregateArg);
                if (!index) {
                    throw std::runtime_error("unknown aggregate column");
                }
                state.argIndex = *index;
            }
            acc.push_back(std::move(state));
        }
        return acc;
    };

    auto observe = [&](const Row &row) {
        Row key;
        key.reserve(groupIndices.size());
        for (const auto index : groupIndices) {
            key.push_back(row[index]);
        }
        auto &group = groups[key];
        if (group.aggregates.empty()) {
            group.key = key;
            group.aggregates = makeAccumulators();
        }
        for (auto &acc : group.aggregates) {
            acc.observe(row);
        }
    };

    if (rows.empty() && command.groupBy.empty() && hasAggregates(command.columns)) {
        GroupState group;
        group.aggregates = makeAccumulators();
        groups.emplace(Row{}, std::move(group));
    } else {
        for (const auto &row : rows) {
            observe(row);
        }
    }

    QueryResult result;
    result.success = true;
    result.message = "selected rows";
    result.columns.reserve(command.columns.size());
    for (const auto &expr : command.columns) {
        result.columns.push_back(expr.outputName());
    }

    for (auto &[_, group] : groups) {
        Row out;
        out.reserve(command.columns.size());
        std::size_t aggregateIndex = 0;
        for (const auto &expr : command.columns) {
            if (expr.kind == SelectExpr::Kind::Column) {
                std::optional<std::size_t> matched;
                for (std::size_t i = 0; i < command.groupBy.size(); ++i) {
                    if (command.groupBy[i] == expr.column ||
                        groupByContains({command.groupBy[i]}, expr.column) ||
                        groupByContains({std::string{expr.column}}, command.groupBy[i])) {
                        matched = i;
                        break;
                    }
                }
                if (!matched) {
                    throw std::runtime_error("selected column must appear in GROUP BY or an aggregate");
                }
                out.push_back(group.key[*matched]);
            } else {
                out.push_back(group.aggregates[aggregateIndex++].result());
            }
        }
        result.rows.push_back(std::move(out));
    }

    return result;
}

Query bindQueryParameters(const Query &query, const std::vector<Value> &parameters) {
    std::vector<bool> seen(parameters.size(), false);
    Query bound = std::visit(
        [&](const auto &command) -> Query {
            using Command = std::decay_t<decltype(command)>;
            if constexpr (std::is_same_v<Command, Select>) {
                return bindSelect(command, parameters, seen);
            } else if constexpr (std::is_same_v<Command, ExplainQuery>) {
                ExplainQuery explained = command;
                explained.query = bindSelect(command.query, parameters, seen);
                return explained;
            } else if constexpr (std::is_same_v<Command, Insert>) {
                Insert insert = command;
                for (auto &row : insert.rows) {
                    for (auto &value : row) {
                        value = bindValue(value, parameters, seen);
                    }
                }
                return insert;
            } else if constexpr (std::is_same_v<Command, Update>) {
                Update update = command;
                update.value = bindValue(command.value, parameters, seen);
                if (update.where) {
                    update.where = bindPredicate(*command.where, parameters, seen);
                }
                return update;
            } else if constexpr (std::is_same_v<Command, Delete>) {
                Delete del = command;
                if (del.where) {
                    del.where = bindPredicate(*command.where, parameters, seen);
                }
                return del;
            } else if constexpr (std::is_same_v<Command, CreateIndex>) {
                CreateIndex index = command;
                if (index.expression) {
                    index.expression = bindIndexExpression(*command.expression, parameters, seen);
                }
                return index;
            } else {
                return command;
            }
        },
        query);

    for (bool wasSeen : seen) {
        if (!wasSeen) {
            throw std::runtime_error("too many prepared statement parameters");
        }
    }
    return bound;
}

} // namespace VertexDB
