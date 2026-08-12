#include "VertexDB/execution/select_helpers.hpp"

#include <algorithm>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

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
                                              std::string_view requested,
                                              std::optional<std::string_view> tableAlias) {
    const auto stripQualifier = [&](std::string_view qualifier) {
        const auto prefix = std::string{qualifier} + ".";
        if (requested.starts_with(prefix)) {
            requested.remove_prefix(prefix.size());
            return true;
        }
        return false;
    };
    if (!stripQualifier(tableName) && tableAlias) {
        (void)stripQualifier(*tableAlias);
    }
    return table.columnIndex(requested);
}

QueryResult messageResult(bool success, std::string message) {
    QueryResult result;
    result.success = success;
    result.message = std::move(message);
    return result;
}

Select mutationScanSelect(std::string table, std::optional<Predicate> where) {
    Select scan;
    scan.table = std::move(table);
    scan.columns = {SelectExpr::makeStar()};
    scan.where = std::move(where);
    return scan;
}

namespace {

[[nodiscard]] std::size_t hashValue(const Value &value) {
    if (value.isNull()) {
        return 0x9e3779b97f4a7c15ULL;
    }
    switch (value.type()) {
    case ColumnType::Int:
        return std::hash<std::int64_t>{}(std::get<std::int64_t>(value.data()));
    case ColumnType::Double:
        return std::hash<double>{}(std::get<double>(value.data()));
    case ColumnType::String:
        return std::hash<std::string>{}(std::get<std::string>(value.data()));
    }
    return 0;
}

void requireCompatibleSetOpSchemas(const QueryResult &left, const QueryResult &right) {
    if (!left.success) {
        throw std::runtime_error(left.message);
    }
    if (!right.success) {
        throw std::runtime_error(right.message);
    }
    if (left.columns.size() != right.columns.size()) {
        throw std::runtime_error("set operation requires equal column counts");
    }
    for (const auto &row : left.rows) {
        if (row.size() != left.columns.size()) {
            throw std::runtime_error("set operation left row width mismatch");
        }
    }
    for (const auto &row : right.rows) {
        if (row.size() != right.columns.size()) {
            throw std::runtime_error("set operation right row width mismatch");
        }
    }
}

} // namespace

std::size_t RowHash::operator()(const Row &row) const {
    std::size_t h = row.size();
    for (const auto &value : row) {
        h ^= hashValue(value) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    }
    return h;
}

std::string_view setOpKindName(SetOpKind op) noexcept {
    switch (op) {
    case SetOpKind::Union:
        return "union";
    case SetOpKind::UnionAll:
        return "union all";
    case SetOpKind::Intersect:
        return "intersect";
    case SetOpKind::IntersectAll:
        return "intersect all";
    case SetOpKind::Except:
        return "except";
    case SetOpKind::ExceptAll:
        return "except all";
    }
    return "set-op";
}

std::vector<Row> deduplicateRows(std::vector<Row> rows) {
    std::unordered_set<Row, RowHash> seen;
    std::vector<Row> unique;
    unique.reserve(rows.size());
    for (auto &row : rows) {
        if (seen.insert(row).second) {
            unique.push_back(std::move(row));
        }
    }
    return unique;
}

QueryResult applySetOperation(SetOpKind op, QueryResult left, QueryResult right) {
    requireCompatibleSetOpSchemas(left, right);
    QueryResult result;
    result.success = true;
    result.message = "selected rows";
    result.columns = std::move(left.columns);

    switch (op) {
    case SetOpKind::UnionAll:
        result.rows = std::move(left.rows);
        result.rows.insert(result.rows.end(), std::make_move_iterator(right.rows.begin()),
                           std::make_move_iterator(right.rows.end()));
        break;
    case SetOpKind::Union: {
        auto combined = std::move(left.rows);
        combined.insert(combined.end(), std::make_move_iterator(right.rows.begin()),
                        std::make_move_iterator(right.rows.end()));
        result.rows = deduplicateRows(std::move(combined));
        break;
    }
    case SetOpKind::Intersect: {
        std::unordered_set<Row, RowHash> rightSet(right.rows.begin(), right.rows.end());
        std::unordered_set<Row, RowHash> emitted;
        for (auto &row : left.rows) {
            if (rightSet.contains(row) && emitted.insert(row).second) {
                result.rows.push_back(std::move(row));
            }
        }
        break;
    }
    case SetOpKind::IntersectAll: {
        // Multiset intersect: emit min(count_left, count_right) copies, left order.
        std::unordered_map<Row, std::size_t, RowHash> rightCounts;
        for (const auto &row : right.rows) {
            ++rightCounts[row];
        }
        for (auto &row : left.rows) {
            auto it = rightCounts.find(row);
            if (it != rightCounts.end() && it->second > 0) {
                --it->second;
                result.rows.push_back(std::move(row));
            }
        }
        break;
    }
    case SetOpKind::Except: {
        std::unordered_set<Row, RowHash> rightSet(right.rows.begin(), right.rows.end());
        std::unordered_set<Row, RowHash> emitted;
        for (auto &row : left.rows) {
            if (!rightSet.contains(row) && emitted.insert(row).second) {
                result.rows.push_back(std::move(row));
            }
        }
        break;
    }
    case SetOpKind::ExceptAll: {
        // Multiset except: emit max(0, count_left - count_right) copies, left order.
        std::unordered_map<Row, std::size_t, RowHash> rightCounts;
        for (const auto &row : right.rows) {
            ++rightCounts[row];
        }
        for (auto &row : left.rows) {
            auto it = rightCounts.find(row);
            if (it != rightCounts.end() && it->second > 0) {
                --it->second;
                continue;
            }
            result.rows.push_back(std::move(row));
        }
        break;
    }
    }
    return result;
}

} // namespace VertexDB
