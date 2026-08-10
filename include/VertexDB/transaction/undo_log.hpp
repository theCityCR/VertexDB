#pragma once

// Per-transaction compensating actions for ROLLBACK.
// Implementation: src/transaction/undo_log.cpp.

#include "VertexDB/storage/row.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace VertexDB {

enum class UndoKind : std::uint8_t {
    Insert,
    Update,
    Delete,
};

struct UndoRecord {
    std::string tableName;
    UndoKind kind{UndoKind::Insert};
    RowId rowId{};
    std::optional<Row> beforeImage;
};

class UndoLog {
  public:
    void push(UndoRecord record);
    void clear();
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] const std::vector<UndoRecord> &entries() const noexcept;

    // Pop the most recently pushed record, or nullopt if empty.
    [[nodiscard]] std::optional<UndoRecord> pop();

  private:
    std::vector<UndoRecord> records_;
};

} // namespace VertexDB
