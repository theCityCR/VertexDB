#pragma once

// Per-transaction compensating actions for ROLLBACK.
// Implementation: src/transaction/undo_log.cpp.

#include "VertexDB/common/index_expression.hpp"
#include "VertexDB/common/value.hpp"
#include "VertexDB/indexing/index_manager.hpp"
#include "VertexDB/parser/predicate.hpp"
#include "VertexDB/storage/foreign_key.hpp"
#include "VertexDB/storage/row.hpp"
#include "VertexDB/storage/unique_constraint.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace VertexDB {

class Database;
class Table;

enum class UndoKind : std::uint8_t {
    Insert,
    Update,
    Delete,
    CreateIndex,
    DropIndex,
    CreateTable,
    DropTable,
    RenameTable,
    SwapDatabase,
    AlterAddColumn,
    AlterDropColumn,
    AlterRenameColumn,
};

struct UndoRecord {
    std::string tableName;
    UndoKind kind{UndoKind::Insert};
    RowId rowId{};
    std::optional<Row> beforeImage;
    // Populated for UndoKind::CreateIndex / DropIndex; empty for DML undo.
    std::string indexName;
    // Populated for UndoKind::DropIndex so ROLLBACK can recreate the index.
    std::string indexColumn;
    std::vector<std::string> indexColumns;
    std::optional<IndexExpression> indexExpression;
    // Populated for UndoKind::DropTable so ROLLBACK can reattach the table.
    std::shared_ptr<Table> retainedTable;
    // Populated for UndoKind::SwapDatabase so ROLLBACK can restore the prior DB.
    std::shared_ptr<Database> previousDatabase;
    // Populated for UndoKind::RenameTable: tableName is the pre-rename name, renameTo the new name.
    std::string renameTo;
    // Populated for UndoKind::AlterAddColumn / AlterDropColumn / AlterRenameColumn.
    Column alterColumn;
    std::size_t alterColumnIndex{};
    std::vector<std::pair<RowId, Value>> alterHeapColumnValues;
    std::vector<std::pair<RowId, std::vector<Value>>> alterVersionColumnValues;
    bool alterCascaded{false};
    std::vector<IndexDefinition> alterDroppedUserIndexes;
    std::vector<Predicate> alterDroppedChecks;
    std::vector<UniqueConstraint> alterDroppedUniques;
    std::vector<ForeignKeyConstraint> alterDroppedChildForeignKeys;
    // AlterRenameColumn: alterColumn.name is old name, renameTo is new name.
};

class UndoLog {
  public:
    void push(UndoRecord record);
    void clear();
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] const std::vector<UndoRecord> &entries() const noexcept;
    void rewriteTableName(std::string_view oldName, std::string_view newName);

    // Pop the most recently pushed record, or nullopt if empty.
    [[nodiscard]] std::optional<UndoRecord> pop();

  private:
    std::vector<UndoRecord> records_;
};

} // namespace VertexDB
