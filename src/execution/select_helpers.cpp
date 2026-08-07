#include "VertexDB/execution/select_helpers.hpp"

#include <algorithm>
#include <stdexcept>

namespace VertexDB {

void sortRowsByColumn(std::vector<Row> &rows, std::size_t columnIndex, bool ascending) {
    std::ranges::sort(rows, [&](const Row &left, const Row &right) {
        if (ascending) {
            return left[columnIndex] < right[columnIndex];
        }
        return right[columnIndex] < left[columnIndex];
    });
}

QueryResult projectWithLimit(std::vector<Row> rows, const std::vector<std::size_t> &projection,
                             std::vector<std::string> columns, std::optional<std::size_t> limit,
                             std::string message) {
    QueryResult result{true, std::move(message), std::move(columns), {}};
    for (const auto &row : rows) {
        Row projected;
        projected.reserve(projection.size());
        for (const auto index : projection) {
            projected.push_back(row[index]);
        }
        result.rows.push_back(std::move(projected));
        if (limit && result.rows.size() >= *limit) {
            break;
        }
    }
    return result;
}

std::optional<std::size_t> resolveResultColumn(std::span<const std::string> columns,
                                               std::string_view requested) {
    for (std::size_t i = 0; i < columns.size(); ++i) {
        if (columns[i] == requested) {
            return i;
        }
    }

    std::optional<std::size_t> match;
    const auto suffix = "." + std::string{requested};
    for (std::size_t i = 0; i < columns.size(); ++i) {
        if (columns[i].size() < suffix.size()) {
            continue;
        }
        if (columns[i].compare(columns[i].size() - suffix.size(), suffix.size(), suffix) == 0) {
            if (match) {
                throw std::runtime_error("ambiguous column reference");
            }
            match = i;
        }
    }
    return match;
}

std::optional<std::size_t> resolveTableColumn(const Table &table, std::string_view tableName,
                                              std::string_view requested) {
    const auto qualifier = std::string{tableName} + ".";
    if (requested.starts_with(qualifier)) {
        requested.remove_prefix(qualifier.size());
    }
    return table.columnIndex(requested);
}

QueryResult messageResult(bool success, std::string message) {
    QueryResult result;
    result.success = success;
    result.message = std::move(message);
    return result;
}

} // namespace VertexDB
