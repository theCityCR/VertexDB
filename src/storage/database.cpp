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
    return static_cast<bool>(detachTable(name));
}

std::shared_ptr<Table> Database::detachTable(std::string_view name) {
    std::unique_lock lock{mutex_};
    auto it = tables_.find(name);
    if (it == tables_.end()) {
        return {};
    }
    auto table = it->second;
    tables_.erase(it);
    return table;
}

bool Database::attachTable(std::shared_ptr<Table> table) {
    if (!table) {
        return false;
    }
    std::unique_lock lock{mutex_};
    auto [_, inserted] = tables_.try_emplace(table->name(), std::move(table));
    return inserted;
}

bool Database::renameTable(std::string_view oldName, std::string newName) {
    std::unique_lock lock{mutex_};
    auto it = tables_.find(oldName);
    if (it == tables_.end() || tables_.contains(newName)) {
        return false;
    }
    auto table = it->second;
    tables_.erase(it);
    const std::string key = newName;
    table->setName(key);
    tables_.emplace(key, std::move(table));
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
        for (const auto &definition : sourceTable->indexDefinitions()) {
            const bool ok =
                definition.expression
                    ? destinationTable->createIndex(definition.name, *definition.expression)
                    : destinationTable->createIndex(definition.name, definition.column);
            if (!ok) {
                throw std::runtime_error("failed to clone index");
            }
        }
        destinationTable->replaceSparse(sourceTable->capacity(), sourceTable->freeList(),
                                        sourceTable->liveEntries());
    }
    return copy;
}

} // namespace VertexDB
