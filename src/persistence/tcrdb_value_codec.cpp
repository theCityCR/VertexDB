#include "tcrdb_detail.hpp"

#include <stdexcept>
#include <variant>

namespace VertexDB::tcrdb_detail {

void writeString(std::ostream &out, std::string_view value) {
    const auto size = static_cast<std::uint64_t>(value.size());
    writePodDb(out, size);
    writeBytesDb(out, value.data(), value.size());
}

std::string readString(std::istream &in) {
    const auto size = readPodDb<std::uint64_t>(in);
    std::string value(size, '\0');
    readBytesDb(in, value.data(), value.size());
    return value;
}

void writeValue(std::ostream &out, const Value &value) {
    if (value.isNull()) {
        writePodDb(out, kNullValueType);
        return;
    }
    writePodDb(out, static_cast<std::uint8_t>(value.type()));
    switch (value.type()) {
    case ColumnType::Int:
        writePodDb(out, std::get<std::int64_t>(value.data()));
        break;
    case ColumnType::Double:
        writePodDb(out, std::get<double>(value.data()));
        break;
    case ColumnType::String:
        writeString(out, std::get<std::string>(value.data()));
        break;
    }
}

Value readValue(std::istream &in) {
    const auto encodedType = readPodDb<std::uint8_t>(in);
    if (encodedType == kNullValueType) {
        return Value{};
    }
    const auto type = static_cast<ColumnType>(encodedType);
    switch (type) {
    case ColumnType::Int:
        return Value{readPodDb<std::int64_t>(in)};
    case ColumnType::Double:
        return Value{readPodDb<double>(in)};
    case ColumnType::String:
        return Value{readString(in)};
    }
    throw std::runtime_error("unsupported value type in database file");
}

Row readRow(std::istream &in, std::size_t columnCount) {
    Row row;
    row.reserve(columnCount);
    for (std::size_t columnIndex = 0; columnIndex < columnCount; ++columnIndex) {
        row.push_back(readValue(in));
    }
    return row;
}

} // namespace VertexDB::tcrdb_detail
