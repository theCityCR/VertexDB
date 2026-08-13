#pragma once

// Append-only WAL records and file I/O. RecoveryService owns replay policy.
// DML redo payloads use page_image_redo (legacy physical_redo still readable).
// Successful append/reset flush and fsync so COMMIT / autocommit WAL durability
// is not left in the OS page cache.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace VertexDB {

enum class WalOperation : std::uint8_t {
    CreateDatabase,
    CreateTable,
    DropTable,
    RenameTable,
    Insert,  // legacy logical SQL; new DML uses PhysicalRedo
    Update,  // legacy logical SQL; new DML uses PhysicalRedo
    Delete,  // legacy logical SQL; new DML uses PhysicalRedo
    CreateIndex,
    SaveDatabase,
    PhysicalRedo,
    PageImageRedo,
    DropIndex,
    DropDatabase,
    AlterTable,
};

struct WalRecord {
    std::uint64_t lsn{};
    WalOperation operation{};
    std::string payload;
};

class WriteAheadLog {
  public:
    explicit WriteAheadLog(std::filesystem::path path);

    // Appends a complete record, then flushes and fsyncs the WAL file (and the
    // parent directory when the file is newly created on POSIX). On Windows, only
    // the file is FlushFileBuffers'd — directory sync is not portable there.
    // Returns only after the record is durable from the engine's perspective.
    [[nodiscard]] std::uint64_t append(WalOperation operation, std::string payload);
    [[nodiscard]] std::vector<WalRecord> readAll() const;
    // Truncates the WAL and durable-syncs the empty file (SAVE checkpoint).
    void reset();

    // Number of successful durable syncs (append or reset). For tests.
    [[nodiscard]] std::uint64_t durableSyncCount() const noexcept { return durableSyncCount_; }

  private:
    [[nodiscard]] std::uint64_t nextLsn();
    void durableSync(bool syncParentDirectory);

    std::filesystem::path path_;
    std::optional<std::uint64_t> nextLsn_;
    std::uint64_t durableSyncCount_{0};
};

} // namespace VertexDB
