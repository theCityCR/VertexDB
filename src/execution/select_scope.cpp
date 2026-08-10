#include "VertexDB/execution/select_scope.hpp"

#include "VertexDB/common/string_utils.hpp"
#include "VertexDB/parser/predicate.hpp"

#include <type_traits>

namespace VertexDB {
namespace {

[[nodiscard]] std::string_view unqualifiedColumn(std::string_view name) {
    const auto dot = name.rfind('.');
    if (dot == std::string_view::npos) {
        return name;
    }
    return name.substr(dot + 1);
}

[[nodiscard]] std::optional<std::string_view> columnQualifier(std::string_view name) {
    const auto dot = name.find('.');
    if (dot == std::string_view::npos) {
        return std::nullopt;
    }
    return name.substr(0, dot);
}

void stripLocalQualifier(std::string &name, std::string_view scope, std::string_view table,
                         bool stripTableName) {
    if (const auto qual = columnQualifier(name)) {
        if (equalsIgnoreCase(*qual, scope) ||
            (stripTableName && equalsIgnoreCase(*qual, table))) {
            name = std::string{unqualifiedColumn(name)};
        }
    }
}

void rewriteQualifierToTable(std::string &name, std::string_view scope, std::string_view table) {
    if (const auto qual = columnQualifier(name)) {
        if (equalsIgnoreCase(*qual, scope)) {
            name = std::string{table} + "." + std::string{unqualifiedColumn(name)};
        }
    }
}

void normalizePredicateScopeQualifiers(Predicate &predicate, std::string_view scope,
                                       std::string_view table, bool stripTableName);

void normalizeNestedSelectsInPredicate(Predicate &predicate);

void rewritePredicateJoinAliases(Predicate &predicate, std::string_view scope,
                                 std::string_view table);

void normalizePredicateScopeQualifiers(Predicate &predicate, std::string_view scope,
                                       std::string_view table, bool stripTableName) {
    std::visit(
        [&](auto &node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, AndPred> || std::is_same_v<T, OrPred>) {
                normalizePredicateScopeQualifiers(*node.left, scope, table, stripTableName);
                normalizePredicateScopeQualifiers(*node.right, scope, table, stripTableName);
            } else if constexpr (std::is_same_v<T, ComparisonPred>) {
                stripLocalQualifier(node.column, scope, table, stripTableName);
                if (node.rhsColumn) {
                    stripLocalQualifier(*node.rhsColumn, scope, table, stripTableName);
                }
            } else if constexpr (std::is_same_v<T, InListPred>) {
                stripLocalQualifier(node.column, scope, table, stripTableName);
            } else if constexpr (std::is_same_v<T, InSubqueryPred>) {
                stripLocalQualifier(node.column, scope, table, stripTableName);
                if (node.subquery) {
                    normalizeSelectScopeQualifiers(*node.subquery);
                }
            } else if constexpr (std::is_same_v<T, ExistsPred>) {
                if (node.subquery) {
                    normalizeSelectScopeQualifiers(*node.subquery);
                }
            }
        },
        predicate);
}

void rewritePredicateJoinAliases(Predicate &predicate, std::string_view scope,
                                 std::string_view table) {
    std::visit(
        [&](auto &node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, AndPred> || std::is_same_v<T, OrPred>) {
                rewritePredicateJoinAliases(*node.left, scope, table);
                rewritePredicateJoinAliases(*node.right, scope, table);
            } else if constexpr (std::is_same_v<T, ComparisonPred>) {
                rewriteQualifierToTable(node.column, scope, table);
                if (node.rhsColumn) {
                    rewriteQualifierToTable(*node.rhsColumn, scope, table);
                }
            } else if constexpr (std::is_same_v<T, InListPred>) {
                rewriteQualifierToTable(node.column, scope, table);
            } else if constexpr (std::is_same_v<T, InSubqueryPred>) {
                rewriteQualifierToTable(node.column, scope, table);
                if (node.subquery) {
                    normalizeSelectScopeQualifiers(*node.subquery);
                }
            } else if constexpr (std::is_same_v<T, ExistsPred>) {
                if (node.subquery) {
                    normalizeSelectScopeQualifiers(*node.subquery);
                }
            }
        },
        predicate);
}

void normalizeNestedSelectsInPredicate(Predicate &predicate) {
    std::visit(
        [&](auto &node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, AndPred> || std::is_same_v<T, OrPred>) {
                normalizeNestedSelectsInPredicate(*node.left);
                normalizeNestedSelectsInPredicate(*node.right);
            } else if constexpr (std::is_same_v<T, InSubqueryPred>) {
                if (node.subquery) {
                    normalizeSelectScopeQualifiers(*node.subquery);
                }
            } else if constexpr (std::is_same_v<T, ExistsPred>) {
                if (node.subquery) {
                    normalizeSelectScopeQualifiers(*node.subquery);
                }
            }
        },
        predicate);
}

void rewriteJoinAliasOnSelect(Select &select, std::string_view scope, std::string_view table) {
    for (auto &expr : select.columns) {
        if (expr.kind == SelectExpr::Kind::Column) {
            rewriteQualifierToTable(expr.column, scope, table);
        } else if (expr.kind == SelectExpr::Kind::Aggregate && expr.aggregateArg) {
            rewriteQualifierToTable(*expr.aggregateArg, scope, table);
        }
    }
    for (auto &column : select.groupBy) {
        rewriteQualifierToTable(column, scope, table);
    }
    if (select.orderBy) {
        rewriteQualifierToTable(select.orderBy->column, scope, table);
    }
    for (auto &join : select.joins) {
        rewriteQualifierToTable(join.leftColumn, scope, table);
        rewriteQualifierToTable(join.rightColumn, scope, table);
    }
    if (select.where) {
        rewritePredicateJoinAliases(*select.where, scope, table);
    }
}

} // namespace

void normalizeSelectScopeQualifiers(Select &select) {
    for (auto &cte : select.ctes) {
        if (cte.body) {
            normalizeSelectScopeQualifiers(*cte.body);
        }
    }

    if (!select.joins.empty()) {
        // Join result columns stay physically qualified (`Employees.id`). Rewrite aliases to those
        // physical qualifiers so SELECT/WHERE/ON can use either form.
        if (select.tableAlias) {
            rewriteJoinAliasOnSelect(select, *select.tableAlias, select.table);
        }
        for (const auto &join : select.joins) {
            if (join.tableAlias) {
                rewriteJoinAliasOnSelect(select, *join.tableAlias, join.table);
            }
        }
        if (select.where) {
            normalizeNestedSelectsInPredicate(*select.where);
        }
        return;
    }

    // Only rewrite alias-qualified names (`e.id`). Never strip the physical table qualifier on
    // join queries (`Employees.id` must stay distinct from `Departments.id`).
    if (select.tableAlias) {
        const std::string_view scope = *select.tableAlias;
        constexpr bool stripTableName = true;
        for (auto &expr : select.columns) {
            if (expr.kind == SelectExpr::Kind::Column) {
                stripLocalQualifier(expr.column, scope, select.table, stripTableName);
            } else if (expr.kind == SelectExpr::Kind::Aggregate && expr.aggregateArg) {
                stripLocalQualifier(*expr.aggregateArg, scope, select.table, stripTableName);
            }
        }
        for (auto &column : select.groupBy) {
            stripLocalQualifier(column, scope, select.table, stripTableName);
        }
        if (select.orderBy) {
            stripLocalQualifier(select.orderBy->column, scope, select.table, stripTableName);
        }
        if (select.where) {
            normalizePredicateScopeQualifiers(*select.where, scope, select.table, stripTableName);
        }
        return;
    }

    if (select.where) {
        normalizeNestedSelectsInPredicate(*select.where);
    }
}

} // namespace VertexDB
