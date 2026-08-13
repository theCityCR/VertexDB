#include "VertexDB/persistence/write_ahead_log.hpp"

#include "VertexDB/common/binary_io.hpp"
#include "VertexDB/persistence/durable_sync.hpp"

#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <string>

namespace VertexDB {
namespace {

constexpr std::uint32_t kWalMagic = 0x54435741; // TCWA
constexpr std::uint32_t kWalVersion = 1;
constexpr std::string_view kWalIoError = "failed to read WAL record";

} // namespace

WriteAheadLog::WriteAheadLog(std::filesystem::path path) : path_(std::move(path)) {}

void WriteAheadLog::durableSync(bool syncParentDirectory) {
    if (durability_ == WalDurability::FlushOnly) {
        // Userspace flush already happened before this call; skip fsync for benches.
        return;
    }
    durableSyncFile(path_);
    if (syncParentDirectory) {
        durableSyncDirectory(path_.parent_path());
    }
    ++durableSyncCount_;
}

std::uint64_t WriteAheadLog::append(WalOperation operation, std::string payload) {
    if (!path_.parent_path().empty()) {
        std::filesystem::create_directories(path_.parent_path());
    }
    const bool created = !std::filesystem::exists(path_);
    const auto lsn = nextLsn();
    {
        std::ofstream out{path_, std::ios::binary | std::ios::app};
        if (!out) {
            throw std::runtime_error("failed to open WAL for append");
        }

        writePod(out, kWalMagic);
        writePod(out, kWalVersion);
        writePod(out, lsn);
        writePod(out, static_cast<std::uint8_t>(operation));
        writePod(out, static_cast<std::uint64_t>(payload.size()));
        out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
        out.flush();
        if (!out) {
            throw std::runtime_error("failed to append WAL record");
        }
    }
    // Stream closed: libc buffers are in the kernel; durableSync pushes to stable storage.
    durableSync(created);
    return lsn;
}

std::vector<WalRecord> WriteAheadLog::readAll() const {
    std::vector<WalRecord> records;
    std::ifstream in{path_, std::ios::binary};
    if (!in) {
        return records;
    }

    // Complete records only: a torn trailing write (crash mid-append) is ignored so recovery can
    // replay the durable prefix. Mid-file corruption also stops reading at the first bad record.
    while (in.peek() != std::ifstream::traits_type::eof()) {
        std::uint32_t magic = 0;
        std::uint32_t version = 0;
        try {
            magic = readPod<std::uint32_t>(in, kWalIoError);
            version = readPod<std::uint32_t>(in, kWalIoError);
        } catch (const std::exception &) {
            break;
        }
        if (magic != kWalMagic || version != kWalVersion) {
            break;
        }

        WalRecord record;
        try {
            record.lsn = readPod<std::uint64_t>(in, kWalIoError);
            record.operation = static_cast<WalOperation>(readPod<std::uint8_t>(in, kWalIoError));
            const auto payloadSize = readPod<std::uint64_t>(in, kWalIoError);
            record.payload.resize(static_cast<std::size_t>(payloadSize));
            in.read(record.payload.data(), static_cast<std::streamsize>(record.payload.size()));
            if (!in || static_cast<std::size_t>(in.gcount()) != record.payload.size()) {
                break;
            }
        } catch (const std::exception &) {
            break;
        }
        records.push_back(std::move(record));
    }
    return records;
}

void WriteAheadLog::reset() {
    if (!path_.parent_path().empty()) {
        std::filesystem::create_directories(path_.parent_path());
    }
    const bool created = !std::filesystem::exists(path_);
    {
        std::ofstream out{path_, std::ios::binary | std::ios::trunc};
        if (!out) {
            throw std::runtime_error("failed to reset WAL");
        }
        out.flush();
        if (!out) {
            throw std::runtime_error("failed to reset WAL");
        }
    }
    durableSync(created);
    nextLsn_ = 1;
}

std::uint64_t WriteAheadLog::nextLsn() {
    if (nextLsn_) {
        return (*nextLsn_)++;
    }

    std::uint64_t next = 1;
    for (const auto &record : readAll()) {
        next = std::max(next, record.lsn + 1);
    }
    nextLsn_ = next + 1;
    return next;
}

} // namespace VertexDB
