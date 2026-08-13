#pragma once

// Typed SQL values (INT / STRING / DOUBLE / NULL / parameter slots / composite index keys).
// Implementation: src/common/value.cpp.

#include <cstdint>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace VertexDB {

enum class ColumnType : std::uint8_t {
    Int,
    Double,
    String,
};

struct ParameterSlot {
    std::size_t index{};

    [[nodiscard]] friend bool operator==(const ParameterSlot &, const ParameterSlot &) = default;
    [[nodiscard]] friend bool operator<(const ParameterSlot &lhs, const ParameterSlot &rhs) {
        return lhs.index < rhs.index;
    }
};

struct CompositeParts;

using ValueData =
    std::variant<std::monostate, std::int64_t, double, std::string, ParameterSlot,
                 std::shared_ptr<const CompositeParts>>;

class Value {
  public:
    Value() = default;
    Value(std::int64_t value);
    Value(int value);
    Value(double value);
    Value(std::string value);

    [[nodiscard]] static Value parameter(std::size_t index);
    [[nodiscard]] static Value composite(std::vector<Value> parts);

    [[nodiscard]] ColumnType type() const;
    [[nodiscard]] bool isNull() const noexcept;
    [[nodiscard]] bool isParameter() const noexcept;
    [[nodiscard]] bool isComposite() const noexcept;
    [[nodiscard]] std::size_t parameterIndex() const;
    [[nodiscard]] const std::vector<Value> &compositeParts() const;
    [[nodiscard]] bool hasNullCompositePart() const noexcept;
    [[nodiscard]] const ValueData &data() const noexcept;
    [[nodiscard]] std::string toString() const;

    friend bool operator==(const Value &lhs, const Value &rhs);
    friend bool operator<(const Value &lhs, const Value &rhs);

  private:
    ValueData data_{std::monostate{}};
};

struct CompositeParts {
    std::vector<Value> parts;

    friend bool operator==(const CompositeParts &lhs, const CompositeParts &rhs);
    friend bool operator<(const CompositeParts &lhs, const CompositeParts &rhs);
};

struct Column {
    std::string name;
    ColumnType type;
    bool nullable{false};
    // ACID Consistency: NOT NULL is the default (!nullable). PRIMARY KEY also sets unique and clears nullable.
    bool unique{false};
    bool primaryKey{false};
};

std::ostream &operator<<(std::ostream &os, const Value &value);
std::string toString(ColumnType type);
std::optional<ColumnType> columnTypeFromString(std::string_view text);

} // namespace VertexDB
