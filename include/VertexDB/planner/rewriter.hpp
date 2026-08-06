#pragma once

#include "VertexDB/parser/ast.hpp"

#include <string>
#include <utility>
#include <vector>

namespace VertexDB {

struct RewriteResult {
    Select query;
    std::vector<std::string> notes;
    // MATERIALIZED CTEs: execute body into an ephemeral table before planning.
    std::vector<std::pair<std::string, Select>> materialize;
};

// Inline WITH CTEs into the main SELECT (default / NOT MATERIALIZED). MATERIALIZED CTEs are
// preserved as ephemeral-table materialization requests. IN subqueries stay for the executor.
[[nodiscard]] RewriteResult rewriteSelect(const Select &query);

} // namespace VertexDB
