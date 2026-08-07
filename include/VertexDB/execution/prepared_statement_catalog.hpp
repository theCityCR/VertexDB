#pragma once

#include "VertexDB/parser/ast.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace VertexDB {

class PreparedStatementCatalog {
  public:
    void store(std::string name, Query query);
    [[nodiscard]] std::optional<Query> find(std::string_view name) const;
    [[nodiscard]] std::optional<Query> findCaseInsensitive(std::string_view name) const;
    [[nodiscard]] bool exists(std::string_view name) const;

  private:
    std::unordered_map<std::string, Query> preparedStatements_;
};

} // namespace VertexDB
