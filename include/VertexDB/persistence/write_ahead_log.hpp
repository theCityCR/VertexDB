#pragma once

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
    Insert,
    Update,
    Delete,
    CreateIndex,
    SaveDatabase,
};

struct WalRecord {
    std::uint64_t lsn{};
    WalOperation operation{};
    std::string payload;
};

class WriteAheadLog {
  public:
    explicit WriteAheadLog(std::filesystem::path path);

    [[nodiscard]] std::uint64_t append(WalOperation operation, std::string payload);
    [[nodiscard]] std::vector<WalRecord> readAll() const;
    void reset();

  private:
    [[nodiscard]] std::uint64_t nextLsn();

    std::filesystem::path path_;
    std::optional<std::uint64_t> nextLsn_;
};

} // namespace VertexDB
