#pragma once

// CTE / derived-table / IN / EXISTS preparation and evaluation, plus full predicate matching.
// Implementation: subquery_runtime.cpp (+ subquery_runtime_bind.cpp, subquery_runtime_cte.cpp).
// Rewrites go through rewriter.hpp first. Shared services and SelectEngine peer live in
// ExecutionContext.
// Allowed peer calls: SubqueryRuntime may call SelectEngine collect/plan helpers only;
// SelectEngine may call SubqueryRuntime::matches only (no mutual recursion through execute).

#include "VertexDB/execution/execution_context.hpp"
#include "VertexDB/execution/query_result.hpp"
#include "VertexDB/parser/ast.hpp"
#include "VertexDB/planner/rewriter.hpp"
#include "VertexDB/storage/table.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace VertexDB {

class SubqueryRuntime {
  public:
    explicit SubqueryRuntime(ExecutionContext &ctx) noexcept;

    [[nodiscard]] Select prepareSelect(const Select &command, RewriteResult &rewrite) const;
    [[nodiscard]] Predicate materializePredicate(const Predicate &predicate) const;
    [[nodiscard]] std::vector<Value> evaluateSubqueryValues(const Select &subquery) const;
    [[nodiscard]] bool evaluateExists(const Select &subquery) const;
    [[nodiscard]] bool matches(const Row &row, const Table &table, const Predicate &predicate,
                               std::string_view scopeName = {}) const;
    [[nodiscard]] Select bindOuterReferences(const Select &subquery, const Row &outerRow,
                                             const Table &outerTable) const;
    [[nodiscard]] Select bindOuterReferences(const Select &subquery, const Row &outerRow,
                                             const Table &outerTable,
                                             std::string_view outerScope) const;
    [[nodiscard]] Predicate bindOuterReferences(const Predicate &predicate, const Row &outerRow,
                                                const Table &outerTable) const;
    [[nodiscard]] Predicate bindOuterReferences(const Predicate &predicate, const Row &outerRow,
                                                const Table &outerTable,
                                                std::string_view outerScope) const;
    [[nodiscard]] std::shared_ptr<Table> materializeCteTable(const CteEntry &cte) const;

  private:
    // Evaluate a SELECT (including set-op chains) to a QueryResult without calling SelectEngine::execute.
    [[nodiscard]] QueryResult evaluateSelectResult(
        const Select &body,
        const std::unordered_map<std::string, std::shared_ptr<Table>> &extraTemps = {}) const;

    ExecutionContext &ctx_;
};

} // namespace VertexDB
