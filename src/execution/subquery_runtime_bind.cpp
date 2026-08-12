#include "VertexDB/execution/subquery_runtime.hpp"

#include "VertexDB/common/string_utils.hpp"
#include "VertexDB/parser/predicate.hpp"

#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>

namespace VertexDB {

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
        if (cte.body) {
            auto body = bindOuterReferences(*cte.body, outerRow, outerTable, outerScope);
            if (body.hasOuterRefs) {
                bound.hasOuterRefs = true;
            }
            cte.body = std::make_shared<Select>(std::move(body));
        }
        if (cte.recursiveArm) {
            auto arm = bindOuterReferences(*cte.recursiveArm, outerRow, outerTable, outerScope);
            if (arm.hasOuterRefs) {
                bound.hasOuterRefs = true;
            }
            cte.recursiveArm = std::make_shared<Select>(std::move(arm));
        }
    }
    for (auto &arm : bound.setOps) {
        if (arm.select) {
            auto right = bindOuterReferences(*arm.select, outerRow, outerTable, outerScope);
            if (right.hasOuterRefs) {
                bound.hasOuterRefs = true;
            }
            arm.select = std::make_shared<Select>(std::move(right));
        }
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

} // namespace VertexDB
