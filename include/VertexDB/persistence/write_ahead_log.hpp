#pragma once

// Append-only WAL records and file I/O. RecoveryService owns replay policy.
// DML redo payloads use page_image_redo (legacy physical_redo still readable).
// Default WalDurability::Sync: successful append/reset flush and fsync so COMMIT /
// autocommit WAL durability is not left in the OS page cache. FlushOnly skips fsync
// for benchmarks — never the silent default.

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

// WAL post-append durability policy. Sync is the educational / production default.
// FlushOnly flushes userspace buffers into the kernel but skips fsync / F_FULLFSYNC /
// FlushFileBuffers — for microbenchmarks only; do not use for correctness demos.
enum class WalDurability : std::uint8_t {
    Sync = 0,
    FlushOnly = 1,
};

struct WalRecord {
    std::uint64_t lsn{};
    WalOperation operation{};
    std::string payload;
};

class WriteAheadLog {
  public:
    explicit WriteAheadLog(std::filesystem::path path);

    void setDurability(WalDurability policy) noexcept { durability_ = policy; }
    [[nodiscard]] WalDurability durability() const noexcept { return durability_; }

    // Appends a complete record, then applies the durability policy. Under Sync,
    // flushes and fsyncs the WAL file (and the parent directory when the file is newly
    // created on POSIX). On Windows, only the file is FlushFileBuffers'd — directory
    // sync is not portable there. Under FlushOnly, skips durable sync after flush.
    // Returns only after the record is durable from the engine's perspective (Sync),
    // or after userspace flush (FlushOnly).
    [[nodiscard]] std::uint64_t append(WalOperation operation, std::string payload);
    [[nodiscard]] std::vector<WalRecord> readAll() const;
    // Truncates the WAL and applies the durability policy (SAVE checkpoint).
    void reset();

    // Number of successful durable (Sync) syncs from append or reset. For tests.
    // FlushOnly appends/resets do not increment this counter.
    [[nodiscard]] std::uint64_t durableSyncCount() const noexcept { return durableSyncCount_; }

  private:
    [[nodiscard]] std::uint64_t nextLsn();
    void durableSync(bool syncParentDirectory);

    std::filesystem::path path_;
    std::optional<std::uint64_t> nextLsn_;
    WalDurability durability_{WalDurability::Sync};
    std::uint64_t durableSyncCount_{0};
};

} // namespace VertexDB
