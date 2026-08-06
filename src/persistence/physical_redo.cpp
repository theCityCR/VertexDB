#include "VertexDB/persistence/physical_redo.hpp"

#include "VertexDB/common/binary_io.hpp"

#include <cstring>
#include <span>
#include <stdexcept>
#include <variant>
#include <vector>

namespace VertexDB {
namespace {

constexpr std::uint8_t kNullValueType = 255;
constexpr std::string_view kTruncated = "truncated physical redo payload";

void appendBytes(std::vector<std::byte> &bytes, const void *data, std::size_t size) {
    const auto *raw = static_cast<const std::byte *>(data);
    bytes.insert(bytes.end(), raw, raw + size);
}

template <typename T> void appendPod(std::vector<std::byte> &bytes, const T &value) {
    appendBytes(bytes, &value, sizeof(T));
}

void appendString(std::vector<std::byte> &bytes, std::string_view value) {
    appendPod(bytes, static_cast<std::uint64_t>(value.size()));
    appendBytes(bytes, value.data(), value.size());
}

void appendValue(std::vector<std::byte> &bytes, const Value &value) {
    if (value.isNull()) {
        appendPod(bytes, kNullValueType);
        return;
    }
    appendPod(bytes, static_cast<std::uint8_t>(value.type()));
    switch (value.type()) {
    case ColumnType::Int:
        appendPod(bytes, std::get<std::int64_t>(value.data()));
        break;
    case ColumnType::Double:
        appendPod(bytes, std::get<double>(value.data()));
        break;
    case ColumnType::String:
        appendString(bytes, std::get<std::string>(value.data()));
        break;
    }
}

std::string readString(std::span<const std::byte> &bytes) {
    const auto size = readPod<std::uint64_t>(bytes, kTruncated);
    if (bytes.size() < size) {
        throw std::runtime_error(std::string(kTruncated));
    }
    std::string value(reinterpret_cast<const char *>(bytes.data()), static_cast<std::size_t>(size));
    bytes = bytes.subspan(static_cast<std::size_t>(size));
    return value;
}

Value readValue(std::span<const std::byte> &bytes) {
    const auto encodedType = readPod<std::uint8_t>(bytes, kTruncated);
    if (encodedType == kNullValueType) {
        return Value{};
    }
    switch (static_cast<ColumnType>(encodedType)) {
    case ColumnType::Int:
        return Value{readPod<std::int64_t>(bytes, kTruncated)};
    case ColumnType::Double:
        return Value{readPod<double>(bytes, kTruncated)};
    case ColumnType::String:
        return Value{readString(bytes)};
    }
    throw std::runtime_error("unsupported value type in physical redo payload");
}

void appendRecord(std::vector<std::byte> &bytes, const PhysicalRedoRecord &record) {
    appendPod(bytes, static_cast<std::uint8_t>(record.kind));
    appendString(bytes, record.tableName);
    appendPod(bytes, static_cast<std::uint64_t>(record.rowId));
    if (record.kind == PhysicalRedoKind::Upsert) {
        appendPod(bytes, static_cast<std::uint64_t>(record.row.size()));
        for (const auto &value : record.row) {
            appendValue(bytes, value);
        }
    } else if (record.kind != PhysicalRedoKind::Erase) {
        throw std::invalid_argument("unknown physical redo kind");
    }
}

PhysicalRedoRecord readRecord(std::span<const std::byte> &bytes) {
    PhysicalRedoRecord record;
    record.kind = static_cast<PhysicalRedoKind>(readPod<std::uint8_t>(bytes, kTruncated));
    record.tableName = readString(bytes);
    record.rowId = static_cast<RowId>(readPod<std::uint64_t>(bytes, kTruncated));
    if (record.kind == PhysicalRedoKind::Upsert) {
        const auto columnCount = readPod<std::uint64_t>(bytes, kTruncated);
        record.row.reserve(static_cast<std::size_t>(columnCount));
        for (std::uint64_t i = 0; i < columnCount; ++i) {
            record.row.push_back(readValue(bytes));
        }
    } else if (record.kind != PhysicalRedoKind::Erase) {
        throw std::runtime_error("unknown physical redo kind in WAL payload");
    }
    return record;
}

} // namespace

std::string encodePhysicalRedos(std::span<const PhysicalRedoRecord> records) {
    if (records.empty()) {
        throw std::invalid_argument("physical redo batch must be non-empty");
    }
    std::vector<std::byte> bytes;
    appendPod(bytes, static_cast<std::uint64_t>(records.size()));
    for (const auto &record : records) {
        appendRecord(bytes, record);
    }
    std::string payload(bytes.size(), '\0');
    std::memcpy(payload.data(), bytes.data(), bytes.size());
    return payload;
}

std::vector<PhysicalRedoRecord> decodePhysicalRedos(std::string_view payload) {
    std::vector<std::byte> storage(payload.size());
    std::memcpy(storage.data(), payload.data(), payload.size());
    std::span<const std::byte> bytes{storage};

    const auto count = readPod<std::uint64_t>(bytes, kTruncated);
    if (count == 0) {
        throw std::runtime_error("empty physical redo batch in WAL payload");
    }
    std::vector<PhysicalRedoRecord> records;
    records.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t i = 0; i < count; ++i) {
        records.push_back(readRecord(bytes));
    }
    if (!bytes.empty()) {
        throw std::runtime_error("trailing bytes in physical redo payload");
    }
    return records;
}

} // namespace VertexDB
