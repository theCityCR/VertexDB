#include "VertexDB/execution/prepared_statement_catalog.hpp"

#include "VertexDB/common/string_utils.hpp"

#include <utility>

namespace VertexDB {

void PreparedStatementCatalog::store(std::string name, Query query) {
    preparedStatements_[std::move(name)] = std::move(query);
}

std::optional<Query> PreparedStatementCatalog::find(std::string_view name) const {
    const auto prepared = preparedStatements_.find(std::string{name});
    if (prepared == preparedStatements_.end()) {
        return std::nullopt;
    }
    return prepared->second;
}

std::optional<Query> PreparedStatementCatalog::findCaseInsensitive(std::string_view name) const {
    for (const auto &[storedName, query] : preparedStatements_) {
        if (equalsIgnoreCase(storedName, name)) {
            return query;
        }
    }
    return std::nullopt;
}

bool PreparedStatementCatalog::exists(std::string_view name) const {
    return preparedStatements_.contains(std::string{name});
}

} // namespace VertexDB
