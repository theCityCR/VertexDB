#include "VertexDB/parser/parser.hpp"

#include "parse_utils.hpp"

#include <stdexcept>

namespace VertexDB {

Select Parser::parseSubquerySelect(bool /*allowOuterRefs*/) {
    if (currentFromTable_.empty()) {
        throw std::runtime_error("subquery is missing an outer FROM scope");
    }
    outerTableStack_.push_back(currentFromTable_);
    const auto savedFrom = currentFromTable_;
    Select subquery;
    if (match(TokenType::Identifier, "WITH")) {
        subquery = parseWithSelectAtDepth(0);
    } else {
        expect(TokenType::Identifier, "SELECT");
        subquery = parseSelectAfterSelectKeyword();
    }
    currentFromTable_ = savedFrom;
    const bool nestedUnderCorrelated = outerTableStack_.size() > 1;
    markOuterRefs(subquery, selectScopeName(subquery), nestedUnderCorrelated);
    outerTableStack_.pop_back();
    return subquery;
}

void Parser::markOuterRefs(Select &select, std::string_view innerTable,
                           bool nestedUnderCorrelated) {
    for (auto &cte : select.ctes) {
        if (cte.body) {
            markOuterRefs(*cte.body, selectScopeName(*cte.body), nestedUnderCorrelated);
            if (cte.body->hasOuterRefs) {
                select.hasOuterRefs = true;
            }
        }
        if (cte.recursiveArm) {
            markOuterRefs(*cte.recursiveArm, selectScopeName(*cte.recursiveArm),
                          nestedUnderCorrelated);
            if (cte.recursiveArm->hasOuterRefs) {
                select.hasOuterRefs = true;
            }
        }
    }
    for (auto &arm : select.setOps) {
        if (arm.select) {
            markOuterRefs(*arm.select, selectScopeName(*arm.select), nestedUnderCorrelated);
            if (arm.select->hasOuterRefs) {
                select.hasOuterRefs = true;
            }
        }
    }
    if (select.where) {
        markOuterRefs(*select.where, innerTable, nestedUnderCorrelated);
        if (predicateReferencesOuter(*select.where)) {
            select.hasOuterRefs = true;
        }
    }
}

void Parser::markOuterRefs(Predicate &predicate, std::string_view innerTable,
                           bool nestedUnderCorrelated) {
    using parser_detail::columnQualifier;
    using parser_detail::refersToOuterTable;
    (void)nestedUnderCorrelated;
    std::visit(
        [&](auto &node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, AndPred> || std::is_same_v<T, OrPred>) {
                markOuterRefs(*node.left, innerTable, nestedUnderCorrelated);
                markOuterRefs(*node.right, innerTable, nestedUnderCorrelated);
            } else if constexpr (std::is_same_v<T, InSubqueryPred> ||
                                 std::is_same_v<T, ExistsPred>) {
                if (node.subquery && node.subquery->hasOuterRefs) {
                    node.referencesOuter = true;
                    if (outerTableStack_.size() > kMaxOuterCorrelationDepth) {
                        throw std::runtime_error(
                            "correlated subquery exceeds maximum outer depth");
                    }
                }
            } else if constexpr (std::is_same_v<T, ComparisonPred>) {
                bool outer = refersToOuterTable(node.column, innerTable, outerTableStack_);
                if (node.rhsColumn) {
                    if (refersToOuterTable(*node.rhsColumn, innerTable, outerTableStack_) ||
                        (!columnQualifier(*node.rhsColumn) && !outerTableStack_.empty())) {
                        outer = true;
                    }
                }
                if (outer) {
                    // Allow up to four outer FROM frames while correlating.
                    if (outerTableStack_.size() > kMaxOuterCorrelationDepth) {
                        throw std::runtime_error(
                            "correlated subquery exceeds maximum outer depth");
                    }
                    node.referencesOuter = true;
                }
            } else if constexpr (std::is_same_v<T, LikePred> || std::is_same_v<T, RegexPred> ||
                                 std::is_same_v<T, InListPred>) {
                if (refersToOuterTable(node.column, innerTable, outerTableStack_)) {
                    if (outerTableStack_.size() > kMaxOuterCorrelationDepth) {
                        throw std::runtime_error(
                            "correlated subquery exceeds maximum outer depth");
                    }
                    node.referencesOuter = true;
                }
            }
        },
        predicate);
}

} // namespace VertexDB
