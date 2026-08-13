#pragma once

// Single-column FOREIGN KEY metadata (NO ACTION / CASCADE / SET NULL).

#include <cstdint>
#include <string>
#include <string_view>

namespace VertexDB {

enum class ForeignKeyAction : std::uint8_t {
    NoAction = 0,
    Cascade = 1,
    SetNull = 2,
};

struct ForeignKeyConstraint {
    std::string childColumn;
    std::string parentTable;
    std::string parentColumn;
    ForeignKeyAction onDelete{ForeignKeyAction::NoAction};
    ForeignKeyAction onUpdate{ForeignKeyAction::NoAction};
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

} // namespace VertexDB
