#pragma once

#include <cstdint>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <variant>

namespace VertexDB {

enum class ColumnType : std::uint8_t {
    Int,
    Double,
    String,
};

using ValueData = std::variant<std::monostate, std::int64_t, double, std::string>;

class Value {
  public:
    Value() = default;
    Value(std::int64_t value);
    Value(int value);
    Value(double value);
    Value(std::string value);

    [[nodiscard]] ColumnType type() const;
    [[nodiscard]] bool isNull() const noexcept;
    [[nodiscard]] const ValueData &data() const noexcept;
    [[nodiscard]] std::string toString() const;

    friend bool operator==(const Value &lhs, const Value &rhs) = default;
    friend bool operator<(const Value &lhs, const Value &rhs);

  private:
    ValueData data_{std::monostate{}};
};

struct Column {
    std::string name;
    ColumnType type;
    bool nullable{false};
};

std::ostream &operator<<(std::ostream &os, const Value &value);
std::string toString(ColumnType type);
std::optional<ColumnType> columnTypeFromString(std::string_view text);

} // namespace VertexDB
