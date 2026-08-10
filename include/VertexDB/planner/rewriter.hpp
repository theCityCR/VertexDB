#pragma once

// CTE / derived-table rewrite (inline or AS MATERIALIZED) before planning.
// Access-path selection lives in query_planner.hpp; execution in SubqueryRuntime.

#include "VertexDB/parser/ast.hpp"

#include <string>
#include <utility>
#include <vector>

namespace VertexDB {

struct RewriteResult {
    Select query;
    std::vector<std::string> notes;
    // MATERIALIZED / recursive CTEs: execute into an ephemeral table before planning.
    std::vector<CteEntry> materialize;
};

// Inline WITH CTEs into the main SELECT (default / NOT MATERIALIZED). MATERIALIZED CTEs are
// preserved as ephemeral-table materialization requests. IN subqueries stay for the executor.
[[nodiscard]] RewriteResult rewriteSelect(const Select &query);

} // namespace VertexDB
