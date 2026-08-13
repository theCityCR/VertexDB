#pragma once

// Versioned .tcrdb snapshot encode/decode (v1–v9). StorageManager owns paths and
// open/rename orchestration; this codec owns the on-disk layout.

#include "VertexDB/storage/database.hpp"

#include <filesystem>
#include <iosfwd>
#include <memory>
#include <string_view>

namespace VertexDB {

inline constexpr std::string_view kTcrdbExtension = ".tcrdb";

[[nodiscard]] std::filesystem::path tcrdbPathFor(const std::filesystem::path &root,
                                                 std::string_view databaseName);
[[nodiscard]] std::filesystem::path tcrdbTemporaryPathFor(const std::filesystem::path &root,
                                                          std::string_view databaseName);

void writeTcrdbSnapshot(std::ostream &out, const Database &database);
[[nodiscard]] std::shared_ptr<Database> readTcrdbSnapshot(std::istream &in);

} // namespace VertexDB
