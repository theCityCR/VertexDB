#pragma once

#include "VertexDB/parser/token.hpp"

#include <string_view>
#include <vector>

namespace VertexDB {

class Tokenizer {
  public:
    [[nodiscard]] std::vector<Token> tokenize(std::string_view sql) const;
};

} // namespace VertexDB
