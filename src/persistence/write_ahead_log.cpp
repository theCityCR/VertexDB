#include "VertexDB/persistence/write_ahead_log.hpp"

#include "VertexDB/common/binary_io.hpp"

#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <string>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace VertexDB {
namespace {

constexpr std::uint32_t kWalMagic = 0x54435741; // TCWA
constexpr std::uint32_t kWalVersion = 1;
constexpr std::string_view kWalIoError = "failed to read WAL record";

[[noreturn]] void throwSyncFailure(const std::filesystem::path &path, const char *what) {
    throw std::runtime_error(std::string(what) + ": " + path.string());
}

#if defined(_WIN32)

void syncFileContents(const std::filesystem::path &path) {
    const HANDLE handle =
        CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        throwSyncFailure(path, "failed to open WAL for durable sync");
    }
    const BOOL ok = FlushFileBuffers(handle);
    CloseHandle(handle);
    if (!ok) {
        throwSyncFailure(path, "failed to durable-sync WAL");
    }
}

void syncDirectory(const std::filesystem::path &dir) {
    // Windows has no reliable FlushFileBuffers for ordinary directories (returns false on
    // GitHub Actions runners). File FlushFileBuffers above is the durable COMMIT contract
    // on Win32; POSIX still fsyncs the parent dir when the WAL file is newly created.
    (void)dir;
}

#else

void syncFileContents(const std::filesystem::path &path) {
    const int fd = ::open(path.c_str(), O_RDWR);
    if (fd < 0) {
        throwSyncFailure(path, "failed to open WAL for durable sync");
    }
#if defined(F_FULLFSYNC)
    // macOS: F_FULLFSYNC is the durable flush; fall back to fsync if unavailable.
    if (::fcntl(fd, F_FULLFSYNC) == -1) {
        if (::fsync(fd) != 0) {
            ::close(fd);
            throwSyncFailure(path, "failed to durable-sync WAL");
        }
    }
#else
    if (::fsync(fd) != 0) {
        ::close(fd);
        throwSyncFailure(path, "failed to durable-sync WAL");
    }
#endif
    if (::close(fd) != 0) {
        throwSyncFailure(path, "failed to close WAL after durable sync");
    }
}

void syncDirectory(const std::filesystem::path &dir) {
    if (dir.empty()) {
        return;
    }
    const int fd = ::open(dir.c_str(), O_RDONLY);
    if (fd < 0) {
        throwSyncFailure(dir, "failed to open WAL directory for durable sync");
    }
#if defined(F_FULLFSYNC)
    if (::fcntl(fd, F_FULLFSYNC) == -1) {
        if (::fsync(fd) != 0) {
            ::close(fd);
            throwSyncFailure(dir, "failed to durable-sync WAL directory");
        }
    }
#else
    if (::fsync(fd) != 0) {
        ::close(fd);
        throwSyncFailure(dir, "failed to durable-sync WAL directory");
    }
#endif
    if (::close(fd) != 0) {
        throwSyncFailure(dir, "failed to close WAL directory after durable sync");
    }
}

#endif

} // namespace

WriteAheadLog::WriteAheadLog(std::filesystem::path path) : path_(std::move(path)) {}

void WriteAheadLog::durableSync(bool syncParentDirectory) {
    syncFileContents(path_);
    if (syncParentDirectory) {
        syncDirectory(path_.parent_path());
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
