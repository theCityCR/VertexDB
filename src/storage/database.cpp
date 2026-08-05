#include "VertexDB/storage/database.hpp"

#include <mutex>
#include <shared_mutex>
#include <stdexcept>

namespace VertexDB {

Database::Database(std::string name) : name_(std::move(name)) {
    if (name_.empty()) {
        throw std::invalid_argument("database name cannot be empty");
    }
}

const std::string &Database::name() const noexcept { return name_; }

bool Database::createTable(std::string name, std::vector<Column> schema) {
    std::unique_lock lock{mutex_};
    auto [_, inserted] =
        tables_.try_emplace(name, std::make_shared<Table>(name, std::move(schema)));
    return inserted;
}

bool Database::dropTable(std::string_view name) {
    std::unique_lock lock{mutex_};
    auto it = tables_.find(name);
    if (it == tables_.end()) {
        return false;
    }
    tables_.erase(it);
    return true;
}

bool Database::renameTable(std::string_view oldName, std::string newName) {
    std::unique_lock lock{mutex_};
    auto it = tables_.find(oldName);
    if (it == tables_.end() || tables_.contains(newName)) {
        return false;
    }
    auto oldTable = it->second;
    tables_.erase(it);
    std::vector<Column> schema{oldTable->schema().begin(), oldTable->schema().end()};
    auto replacement = std::make_shared<Table>(newName, std::move(schema));
    for (const auto &row : oldTable->rowsSnapshot()) {
        replacement->insert(row);
    }
    for (const auto &[indexName, columnName] : oldTable->indexDefinitions()) {
        replacement->createIndex(indexName, columnName);
    }
    tables_.emplace(std::move(newName), std::move(replacement));
    return true;
}

std::shared_ptr<Table> Database::table(std::string_view name) const {
    std::shared_lock lock{mutex_};
    auto it = tables_.find(name);
    if (it == tables_.end()) {
        return {};
    }
    return it->second;
}

std::vector<std::string> Database::listTables() const {
    std::shared_lock lock{mutex_};
    std::vector<std::string> names;
    names.reserve(tables_.size());
    for (const auto &[name, _] : tables_) {
        names.push_back(name);
    }
    return names;
}

std::vector<std::shared_ptr<Table>> Database::tables() const {
    std::shared_lock lock{mutex_};
    std::vector<std::shared_ptr<Table>> result;
    result.reserve(tables_.size());
    for (const auto &[_, table] : tables_) {
        result.push_back(table);
    }
    return result;
}

std::shared_ptr<Database> Database::clone() const {
    auto copy = std::make_shared<Database>(name_);
    for (const auto &sourceTable : tables()) {
        std::vector<Column> schema{sourceTable->schema().begin(), sourceTable->schema().end()};
        const bool created = copy->createTable(sourceTable->name(), std::move(schema));
        if (!created) {
            throw std::runtime_error("failed to clone table");
        }
        auto destinationTable = copy->table(sourceTable->name());
        destinationTable->replaceRows(sourceTable->rowsSnapshot());
        for (const auto &[indexName, columnName] : sourceTable->indexDefinitions()) {
            destinationTable->createIndex(indexName, columnName);
        }
    }
    return copy;
}

} // namespace VertexDB
