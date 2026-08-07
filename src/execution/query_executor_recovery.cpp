#include "VertexDB/execution/query_executor.hpp"

#include <utility>

namespace VertexDB {

void QueryExecutor::appendWal(WalOperation operation, std::string payload) {
    recovery_.appendWal(operation, std::move(payload));
}

void QueryExecutor::appendPageImageRedo(Table &table, std::string tableName) {
    recovery_.appendPageImageRedo(table, std::move(tableName));
}

void QueryExecutor::flushPendingWal() { recovery_.flushPendingWal(); }

void QueryExecutor::clearPendingWal() noexcept { recovery_.clearPendingWal(); }

} // namespace VertexDB
