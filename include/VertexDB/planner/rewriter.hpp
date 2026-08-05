#pragma once

#include "VertexDB/parser/ast.hpp"

#include <string>
#include <vector>

namespace VertexDB {

struct RewriteResult {
    Select query;
    std::vector<std::string> notes;
};

// Inline WITH CTEs into the main SELECT and leave IN subqueries for materialization.
[[nodiscard]] RewriteResult rewriteSelect(const Select &query);

} // namespace VertexDB
