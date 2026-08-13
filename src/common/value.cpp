#include "VertexDB/common/value.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace VertexDB {

bool operator==(const CompositeParts &lhs, const CompositeParts &rhs) {
    return lhs.parts == rhs.parts;
}

bool operator<(const CompositeParts &lhs, const CompositeParts &rhs) {
    return lhs.parts < rhs.parts;
}

Value::Value(std::int64_t value) : data_(value) {}

Value::Value(int value) : data_(static_cast<std::int64_t>(value)) {}

Value::Value(double value) : data_(value) {}

Value::Value(std::string value) : data_(std::move(value)) {}

Value Value::parameter(std::size_t index) {
    Value value;
    value.data_ = ParameterSlot{index};
    return value;
}

Value Value::composite(std::vector<Value> parts) {
    if (parts.empty()) {
        throw std::invalid_argument("composite value requires at least one part");
    }
    if (parts.size() == 1) {
        return std::move(parts.front());
    }
    Value value;
    value.data_ = std::make_shared<const CompositeParts>(
        CompositeParts{std::move(parts)});
    return value;
}

ColumnType Value::type() const {
    if (isNull()) {
        throw std::runtime_error("null value has no concrete column type");
    }
    if (isParameter()) {
        throw std::runtime_error("parameter placeholder has no concrete column type");
    }
    if (isComposite()) {
        throw std::runtime_error("composite value has no concrete column type");
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

bool Value::isParameter() const noexcept { return std::holds_alternative<ParameterSlot>(data_); }

bool Value::isComposite() const noexcept {
    return std::holds_alternative<std::shared_ptr<const CompositeParts>>(data_);
}

std::size_t Value::parameterIndex() const {
    if (!isParameter()) {
        throw std::runtime_error("value is not a parameter placeholder");
    }
    return std::get<ParameterSlot>(data_).index;
}

const std::vector<Value> &Value::compositeParts() const {
    if (!isComposite()) {
        throw std::runtime_error("value is not a composite key");
    }
    return std::get<std::shared_ptr<const CompositeParts>>(data_)->parts;
}

bool Value::hasNullCompositePart() const noexcept {
    if (!isComposite()) {
        return isNull();
    }
    for (const auto &part : compositeParts()) {
        if (part.isNull() || part.hasNullCompositePart()) {
            return true;
        }
    }
    return false;
}

const ValueData &Value::data() const noexcept { return data_; }

std::string Value::toString() const {
    if (isNull()) {
        return "NULL";
    }
    if (isParameter()) {
        return "?";
    }
    if (isComposite()) {
        std::ostringstream stream;
        stream << "(";
        const auto &parts = compositeParts();
        for (std::size_t i = 0; i < parts.size(); ++i) {
            if (i != 0) {
                stream << ", ";
            }
            stream << parts[i].toString();
        }
        stream << ")";
        return stream.str();
    }
    return std::visit(
        [](const auto &item) {
            std::ostringstream stream;
            if constexpr (!std::is_same_v<std::decay_t<decltype(item)>, std::monostate> &&
                          !std::is_same_v<std::decay_t<decltype(item)>, ParameterSlot> &&
                          !std::is_same_v<std::decay_t<decltype(item)>,
                                         std::shared_ptr<const CompositeParts>>) {
                stream << item;
            }
            return stream.str();
        },
        data_);
}

bool operator==(const Value &lhs, const Value &rhs) {
    if (lhs.isComposite() && rhs.isComposite()) {
        return lhs.compositeParts() == rhs.compositeParts();
    }
    if (lhs.isComposite() || rhs.isComposite()) {
        return false;
    }
    return lhs.data_ == rhs.data_;
}

bool operator<(const Value &lhs, const Value &rhs) {
    if (lhs.isNull() || rhs.isNull() || lhs.isParameter() || rhs.isParameter() ||
        lhs.isComposite() || rhs.isComposite()) {
        if (lhs.isComposite() && rhs.isComposite()) {
            return lhs.compositeParts() < rhs.compositeParts();
        }
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
