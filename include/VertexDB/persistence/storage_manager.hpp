#pragma once

// Snapshot path orchestration (open/rename/exists) with durable publish.
// On-disk .tcrdb layout is in tcrdb_codec.hpp. Successful saveDatabase flushes
// and fsyncs the temp snapshot, renames into place, then durable-syncs the
// storage directory (POSIX) — mirroring WAL commit discipline.

#include "VertexDB/storage/database.hpp"

#include <cstdint>
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

    // Number of successful durable snapshot publishes. For tests.
    [[nodiscard]] std::uint64_t durablePublishCount() const noexcept {
        return durablePublishCount_;
    }

  private:
    std::filesystem::path root_;
    mutable std::uint64_t durablePublishCount_{0};
};

} // namespace VertexDB
