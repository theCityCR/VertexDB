#pragma once

// Platform durable sync for files and directories (WAL + snapshot publish).
// POSIX: fsync / F_FULLFSYNC. Windows: FlushFileBuffers on files; directory sync is a no-op.

#include <filesystem>

namespace VertexDB {

// Push file contents to stable storage. Throws std::runtime_error on failure.
void durableSyncFile(const std::filesystem::path &path);

// Make directory entries durable on POSIX. No-op on Windows (not portable there).
void durableSyncDirectory(const std::filesystem::path &dir);

} // namespace VertexDB
