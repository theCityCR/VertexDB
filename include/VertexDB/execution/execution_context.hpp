#pragma once

// Shared execution services for SelectEngine and SubqueryRuntime.
// QueryExecutor owns the members; engines hold this context and peer pointers
// wired after both engines are constructed (no QueryExecutor friendship).

#include "VertexDB/execution/txn_session.hpp"
#include "VertexDB/planner/query_planner.hpp"
#include "VertexDB/storage/database.hpp"

#include <memory>

namespace VertexDB {

class SelectEngine;
class SubqueryRuntime;

struct ExecutionContext {
    std::shared_ptr<Database> &database;
    QueryPlanner &planner;
    TxnSession &session;
    SelectEngine *select = nullptr;
    SubqueryRuntime *subquery = nullptr;

    [[nodiscard]] ReadSnapshot readSnapshot() const { return session.readSnapshot(); }
};

} // namespace VertexDB
