#pragma once

// Single-column FOREIGN KEY metadata (NO ACTION / reject only for v1).

#include <cstdint>
#include <string>

namespace VertexDB {

enum class ForeignKeyAction : std::uint8_t {
    NoAction = 0,
};

struct ForeignKeyConstraint {
    std::string childColumn;
    std::string parentTable;
    std::string parentColumn;
    ForeignKeyAction onDelete{ForeignKeyAction::NoAction};
    ForeignKeyAction onUpdate{ForeignKeyAction::NoAction};
};

} // namespace VertexDB
