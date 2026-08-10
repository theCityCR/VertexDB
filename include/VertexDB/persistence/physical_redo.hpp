#pragma once

// Legacy physical row-image WAL redo codec.
// Implementation: src/persistence/physical_redo.cpp.

#include "VertexDB/storage/row.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace VertexDB {

enum class PhysicalRedoKind : std::uint8_t {
    Upsert = 0,
    Erase = 1,
};

struct PhysicalRedoRecord {
    PhysicalRedoKind kind{PhysicalRedoKind::Upsert};
    std::string tableName;
    RowId rowId{};
    Row row; // after-image for Upsert; empty for Erase
};

[[nodiscard]] std::string encodePhysicalRedos(std::span<const PhysicalRedoRecord> records);
[[nodiscard]] std::vector<PhysicalRedoRecord> decodePhysicalRedos(std::string_view payload);

[[nodiscard]] inline std::string encodePhysicalRedo(const PhysicalRedoRecord &record) {
    return encodePhysicalRedos(std::span<const PhysicalRedoRecord>{&record, 1});
}

} // namespace VertexDB
