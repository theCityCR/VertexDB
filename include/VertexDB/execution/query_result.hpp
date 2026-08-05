#pragma once

#include "VertexDB/storage/row.hpp"

#include <string>
#include <vector>

namespace VertexDB {

struct QueryResult {
    bool success{true};
    std::string message;
    std::vector<std::string> columns;
    std::vector<Row> rows;
};

} // namespace VertexDB
