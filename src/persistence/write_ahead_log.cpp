#include "VertexDB/persistence/write_ahead_log.hpp"

#include <algorithm>
#include <fstream>
#include <stdexcept>

namespace VertexDB {
namespace {

constexpr std::uint32_t kWalMagic = 0x54435741; // TCWA
constexpr std::uint32_t kWalVersion = 1;

template <typename T> void writePod(std::ostream &out, const T &value) {
    out.write(reinterpret_cast<const char *>(&value), sizeof(T));
}

template <typename T> T readPod(std::istream &in) {
    T value{};
    in.read(reinterpret_cast<char *>(&value), sizeof(T));
    if (!in) {
        throw std::runtime_error("failed to read WAL record");
    }
    return value;
}

} // namespace

WriteAheadLog::WriteAheadLog(std::filesystem::path path) : path_(std::move(path)) {}

std::uint64_t WriteAheadLog::append(WalOperation operation, std::string payload) {
    if (!path_.parent_path().empty()) {
        std::filesystem::create_directories(path_.parent_path());
    }
    const auto lsn = nextLsn();
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
    if (!out) {
        throw std::runtime_error("failed to append WAL record");
    }
    return lsn;
}

std::vector<WalRecord> WriteAheadLog::readAll() const {
    std::vector<WalRecord> records;
    std::ifstream in{path_, std::ios::binary};
    if (!in) {
        return records;
    }

    while (in.peek() != std::ifstream::traits_type::eof()) {
        const auto magic = readPod<std::uint32_t>(in);
        const auto version = readPod<std::uint32_t>(in);
        if (magic != kWalMagic || version != kWalVersion) {
            throw std::runtime_error("invalid WAL record header");
        }
        WalRecord record;
        record.lsn = readPod<std::uint64_t>(in);
        record.operation = static_cast<WalOperation>(readPod<std::uint8_t>(in));
        const auto payloadSize = readPod<std::uint64_t>(in);
        record.payload.resize(static_cast<std::size_t>(payloadSize));
        in.read(record.payload.data(), static_cast<std::streamsize>(record.payload.size()));
        if (!in) {
            throw std::runtime_error("truncated WAL payload");
        }
        records.push_back(std::move(record));
    }
    return records;
}

void WriteAheadLog::reset() {
    if (!path_.parent_path().empty()) {
        std::filesystem::create_directories(path_.parent_path());
    }
    std::ofstream out{path_, std::ios::binary | std::ios::trunc};
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
