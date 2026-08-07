#pragma once

// Strip FROM-scope qualifiers (`e.id` → `id`) for planning and execution.
// Implementation: select_scope.cpp.

#include "VertexDB/parser/ast.hpp"

namespace VertexDB {

void normalizeSelectScopeQualifiers(Select &select);

} // namespace VertexDB
