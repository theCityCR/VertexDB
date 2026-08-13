#pragma once

// INSERT / UPDATE / DELETE execution with undo + page-image WAL redo.
// Implementation: dml_engine.cpp. QueryExecutor remains the public façade.

#include "VertexDB/execution/execution_context.hpp"
#include "VertexDB/execution/query_result.hpp"
#include "VertexDB/execution/recovery_service.hpp"
#include "VertexDB/parser/ast.hpp"
#include "VertexDB/storage/row.hpp"
#include "VertexDB/storage/table.hpp"

#include <cstddef>
#include <string>
#include <unordered_set>
#include <utility>

namespace VertexDB {

class DmlEngine {
  public:
    DmlEngine(ExecutionContext &ctx, RecoveryService &recovery) noexcept;

    [[nodiscard]] QueryResult executeInsert(const Insert &command);
    [[nodiscard]] QueryResult executeUpdate(const Update &command);
    [[nodiscard]] QueryResult executeDelete(const Delete &command);

  private:
    void appendPageImageRedo(Table &table, std::string tableName);
    void eraseRowWithReferentialActions(Table &table, std::string tableName, RowId rowId, Row row,
                                        std::size_t depth,
                                        std::unordered_set<std::string> &visiting,
                                        std::unordered_set<std::string> &deleted);

    ExecutionContext &ctx_;
    RecoveryService &recovery_;
};

} // namespace VertexDB
