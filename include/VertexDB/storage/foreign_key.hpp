#pragma once

// FOREIGN KEY metadata (single- or multi-column; NO ACTION / CASCADE / SET NULL).

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace VertexDB {

enum class ForeignKeyAction : std::uint8_t {
    NoAction = 0,
    Cascade = 1,
    SetNull = 2,
};

struct ForeignKeyConstraint {
    std::vector<std::string> childColumns;
    std::string parentTable;
    std::vector<std::string> parentColumns;
    ForeignKeyAction onDelete{ForeignKeyAction::NoAction};
    ForeignKeyAction onUpdate{ForeignKeyAction::NoAction};

    ForeignKeyConstraint() = default;

    // Single-column convenience (column REFERENCES / tests).
    ForeignKeyConstraint(std::string childColumn, std::string parentTableName,
                         std::string parentColumn,
                         ForeignKeyAction onDeleteAction = ForeignKeyAction::NoAction,
                         ForeignKeyAction onUpdateAction = ForeignKeyAction::NoAction)
        : childColumns{std::move(childColumn)}, parentTable{std::move(parentTableName)},
          parentColumns{std::move(parentColumn)}, onDelete{onDeleteAction},
          onUpdate{onUpdateAction} {}
};

[[nodiscard]] inline std::string_view foreignKeyActionName(ForeignKeyAction action) {
    switch (action) {
    case ForeignKeyAction::NoAction:
        return "NO ACTION";
    case ForeignKeyAction::Cascade:
        return "CASCADE";
    case ForeignKeyAction::SetNull:
        return "SET NULL";
    }
    return "NO ACTION";
}

[[nodiscard]] inline std::string foreignKeyColumnsLabel(const std::vector<std::string> &columns) {
    std::string label;
    for (std::size_t i = 0; i < columns.size(); ++i) {
        if (i > 0) {
            label += ", ";
        }
        label += columns[i];
    }
    return label;
}

} // namespace VertexDB
