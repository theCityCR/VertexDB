#pragma once

// Snapshot path orchestration (open/rename/exists). On-disk .tcrdb layout is
// in tcrdb_codec.hpp.

#include "VertexDB/storage/database.hpp"

#include <filesystem>
#include <memory>
#include <string_view>

namespace VertexDB {

class StorageManager {
  public:
    explicit StorageManager(std::filesystem::path root);

    void saveDatabase(const Database &database) const;
    // Removes the on-disk `.tcrdb` snapshot when present; no-op if missing.
    void deleteDatabase(std::string_view databaseName) const;
    [[nodiscard]] std::shared_ptr<Database> loadDatabase(std::string_view databaseName) const;
    [[nodiscard]] std::shared_ptr<Database> loadFirstDatabase() const;
    [[nodiscard]] bool metadataExists(std::string_view databaseName) const;

  private:
    std::filesystem::path root_;
};

} // namespace VertexDB
