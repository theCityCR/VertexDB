#include "VertexDB/persistence/durable_sync.hpp"

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

[[noreturn]] void throwSyncFailure(const std::filesystem::path &path, const char *what) {
    throw std::runtime_error(std::string(what) + ": " + path.string());
}

} // namespace

#if defined(_WIN32)

void durableSyncFile(const std::filesystem::path &path) {
    const HANDLE handle =
        CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        throwSyncFailure(path, "failed to open file for durable sync");
    }
    const BOOL ok = FlushFileBuffers(handle);
    CloseHandle(handle);
    if (!ok) {
        throwSyncFailure(path, "failed to durable-sync file");
    }
}

void durableSyncDirectory(const std::filesystem::path &dir) {
    // Windows has no reliable FlushFileBuffers for ordinary directories (returns false on
    // GitHub Actions runners). File FlushFileBuffers is the durable contract on Win32;
    // POSIX still fsyncs directories after create/rename.
    (void)dir;
}

#else

void durableSyncFile(const std::filesystem::path &path) {
    const int fd = ::open(path.c_str(), O_RDWR);
    if (fd < 0) {
        throwSyncFailure(path, "failed to open file for durable sync");
    }
#if defined(F_FULLFSYNC)
    // macOS: F_FULLFSYNC is the durable flush; fall back to fsync if unavailable.
    if (::fcntl(fd, F_FULLFSYNC) == -1) {
        if (::fsync(fd) != 0) {
            ::close(fd);
            throwSyncFailure(path, "failed to durable-sync file");
        }
    }
#else
    if (::fsync(fd) != 0) {
        ::close(fd);
        throwSyncFailure(path, "failed to durable-sync file");
    }
#endif
    if (::close(fd) != 0) {
        throwSyncFailure(path, "failed to close file after durable sync");
    }
}

void durableSyncDirectory(const std::filesystem::path &dir) {
    if (dir.empty()) {
        return;
    }
    const int fd = ::open(dir.c_str(), O_RDONLY);
    if (fd < 0) {
        throwSyncFailure(dir, "failed to open directory for durable sync");
    }
#if defined(F_FULLFSYNC)
    if (::fcntl(fd, F_FULLFSYNC) == -1) {
        if (::fsync(fd) != 0) {
            ::close(fd);
            throwSyncFailure(dir, "failed to durable-sync directory");
        }
    }
#else
    if (::fsync(fd) != 0) {
        ::close(fd);
        throwSyncFailure(dir, "failed to durable-sync directory");
    }
#endif
    if (::close(fd) != 0) {
        throwSyncFailure(dir, "failed to close directory after durable sync");
    }
}

#endif

} // namespace VertexDB
