#pragma once

// POD read/write helpers for streams and byte spans (header-only).

#include <cstddef>
#include <cstring>
#include <istream>
#include <ostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace VertexDB {

inline void writeBytes(std::ostream &out, const void *data, std::size_t size,
                       std::string_view errorMessage = "failed to write binary data") {
    out.write(static_cast<const char *>(data), static_cast<std::streamsize>(size));
    if (!out) {
        throw std::runtime_error(std::string(errorMessage));
    }
}

inline void readBytes(std::istream &in, void *data, std::size_t size,
                      std::string_view errorMessage = "failed to read binary data") {
    in.read(static_cast<char *>(data), static_cast<std::streamsize>(size));
    if (!in) {
        throw std::runtime_error(std::string(errorMessage));
    }
}

template <typename T>
void writePod(std::ostream &out, const T &value,
              std::string_view errorMessage = "failed to write binary data") {
    writeBytes(out, &value, sizeof(T), errorMessage);
}

template <typename T>
[[nodiscard]] T readPod(std::istream &in,
                        std::string_view errorMessage = "failed to read binary data") {
    T value{};
    readBytes(in, &value, sizeof(T), errorMessage);
    return value;
}

template <typename T>
[[nodiscard]] T readPod(std::span<const std::byte> &bytes,
                        std::string_view errorMessage = "truncated binary payload") {
    if (bytes.size() < sizeof(T)) {
        throw std::runtime_error(std::string(errorMessage));
    }
    T value{};
    std::memcpy(&value, bytes.data(), sizeof(T));
    bytes = bytes.subspan(sizeof(T));
    return value;
}

} // namespace VertexDB
