#pragma once

// Parser diagnostics with source line/column.

#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>

namespace VertexDB {

// Parser/tokenizer failure with 1-based line/column and 0-based byte offset.
class ParseError : public std::runtime_error {
  public:
    ParseError(std::string_view message, std::size_t line, std::size_t column, std::size_t offset)
        : std::runtime_error(format(message, line, column)), line_(line), column_(column),
          offset_(offset) {}

    [[nodiscard]] std::size_t line() const noexcept { return line_; }
    [[nodiscard]] std::size_t column() const noexcept { return column_; }
    [[nodiscard]] std::size_t offset() const noexcept { return offset_; }

  private:
    [[nodiscard]] static std::string format(std::string_view message, std::size_t line,
                                            std::size_t column) {
        return "line " + std::to_string(line) + ", column " + std::to_string(column) + ": " +
               std::string{message};
    }

    std::size_t line_;
    std::size_t column_;
    std::size_t offset_;
};

} // namespace VertexDB
