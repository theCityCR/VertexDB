#include "VertexDB/common/value.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <stdexcept>
#include <type_traits>

namespace VertexDB {

Value::Value(std::int64_t value) : data_(value) {}

Value::Value(int value) : data_(static_cast<std::int64_t>(value)) {}

Value::Value(double value) : data_(value) {}

Value::Value(std::string value) : data_(std::move(value)) {}

ColumnType Value::type() const {
    if (isNull()) {
        throw std::runtime_error("null value has no concrete column type");
    }
    if (std::holds_alternative<std::int64_t>(data_)) {
        return ColumnType::Int;
    }
    if (std::holds_alternative<double>(data_)) {
        return ColumnType::Double;
    }
    return ColumnType::String;
}

bool Value::isNull() const noexcept { return std::holds_alternative<std::monostate>(data_); }

const ValueData &Value::data() const noexcept { return data_; }

std::string Value::toString() const {
    if (isNull()) {
        return "NULL";
    }
    return std::visit(
        [](const auto &item) {
            std::ostringstream stream;
            if constexpr (!std::is_same_v<std::decay_t<decltype(item)>, std::monostate>) {
                stream << item;
            }
            return stream.str();
        },
        data_);
}

bool operator<(const Value &lhs, const Value &rhs) {
    if (lhs.isNull() || rhs.isNull()) {
        return lhs.data_.index() < rhs.data_.index();
    }
    if (lhs.type() != rhs.type()) {
        return static_cast<int>(lhs.type()) < static_cast<int>(rhs.type());
    }
    return lhs.data_ < rhs.data_;
}

std::ostream &operator<<(std::ostream &os, const Value &value) {
    os << value.toString();
    return os;
}

std::string toString(ColumnType type) {
    switch (type) {
    case ColumnType::Int:
        return "INT";
    case ColumnType::Double:
        return "DOUBLE";
    case ColumnType::String:
        return "STRING";
    }
    return "UNKNOWN";
}

std::optional<ColumnType> columnTypeFromString(std::string_view text) {
    std::string normalized{text};
    std::ranges::transform(normalized, normalized.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });

    if (normalized == "INT") {
        return ColumnType::Int;
    }
    if (normalized == "DOUBLE") {
        return ColumnType::Double;
    }
    if (normalized == "STRING") {
        return ColumnType::String;
    }
    return std::nullopt;
}

} // namespace VertexDB
