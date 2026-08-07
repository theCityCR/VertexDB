#include "VertexDB/execution/prepared_bind.hpp"

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

} // namespace

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
