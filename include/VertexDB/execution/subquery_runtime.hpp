#pragma once

#include "VertexDB/parser/ast.hpp"
#include "VertexDB/planner/rewriter.hpp"
#include "VertexDB/storage/table.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace VertexDB {

class QueryExecutor;

class SubqueryRuntime {
  public:
    explicit SubqueryRuntime(QueryExecutor &owner) noexcept;

    [[nodiscard]] Select prepareSelect(const Select &command, RewriteResult &rewrite) const;
    [[nodiscard]] Predicate materializePredicate(const Predicate &predicate) const;
    [[nodiscard]] std::vector<Value> evaluateSubqueryValues(const Select &subquery) const;
    [[nodiscard]] bool evaluateExists(const Select &subquery) const;
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
    [[nodiscard]] std::shared_ptr<Table> materializeCteTable(const std::string &name,
                                                            const Select &body) const;

  private:
    QueryExecutor &owner_;
};

} // namespace VertexDB
