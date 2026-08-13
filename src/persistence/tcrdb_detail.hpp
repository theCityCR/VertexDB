#pragma once

// Internal .tcrdb layout constants and helpers. Used by tcrdb_*_codec.cpp TUs.
// Not part of the public include surface.

#include "VertexDB/common/binary_io.hpp"
#include "VertexDB/persistence/tcrdb_codec.hpp"

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <string>
#include <string_view>

namespace VertexDB::tcrdb_detail {

inline constexpr std::string_view kMagic = "TCRDB001";
inline constexpr std::uint32_t kVersionV1 = 1;
inline constexpr std::uint32_t kVersionV2 = 2;
inline constexpr std::uint32_t kVersionV3 = 3;
inline constexpr std::uint32_t kVersionV4 = 4;
inline constexpr std::uint32_t kVersionV5 = 5;
inline constexpr std::uint32_t kVersion = 6;
inline constexpr std::uint8_t kNullValueType = 255;
inline constexpr std::string_view kWriteError = "failed to write database file";
inline constexpr std::string_view kReadError = "failed to read database file";
inline constexpr std::string_view kHistogramMagic = "VDBHIST1";

template <typename T> void writePodDb(std::ostream &out, const T &value) {
    writePod(out, value, kWriteError);
}

template <typename T> [[nodiscard]] T readPodDb(std::istream &in) {
    return readPod<T>(in, kReadError);
}

inline void writeBytesDb(std::ostream &out, const void *data, std::size_t size) {
    writeBytes(out, data, size, kWriteError);
}

inline void readBytesDb(std::istream &in, void *data, std::size_t size) {
    readBytes(in, data, size, kReadError);
}

void writeString(std::ostream &out, std::string_view value);
[[nodiscard]] std::string readString(std::istream &in);
void writeValue(std::ostream &out, const Value &value);
[[nodiscard]] Value readValue(std::istream &in);
[[nodiscard]] Row readRow(std::istream &in, std::size_t columnCount);

void loadDenseRows(Table &table, std::istream &in, std::size_t columnCount);
void loadSparseRows(Table &table, std::istream &in, std::size_t columnCount);
void loadPagePayloadRows(Table &table, std::istream &in, bool rebuildIndexes);
void writePagePayloadRows(std::ostream &out, const Table &table);

void writeIndexPages(std::ostream &out, const Table &table);
void loadIndexPages(Table &table, std::istream &in);
void writeColumnHistograms(std::ostream &out, const Table &table);
[[nodiscard]] bool tryLoadColumnHistograms(Table &table, std::istream &in);

} // namespace VertexDB::tcrdb_detail
