#include "VertexDB/persistence/storage_manager.hpp"

#include "VertexDB/persistence/tcrdb_codec.hpp"

#include <fstream>
#include <stdexcept>

namespace VertexDB {

StorageManager::StorageManager(std::filesystem::path root) : root_(std::move(root)) {}

void StorageManager::saveDatabase(const Database &database) const {
    std::filesystem::create_directories(root_);
    const auto targetPath = tcrdbPathFor(root_, database.name());
    const auto tempPath = tcrdbTemporaryPathFor(root_, database.name());
    {
        std::ofstream out{tempPath, std::ios::binary | std::ios::trunc};
        if (!out) {
            throw std::runtime_error("failed to open temporary database file for writing");
        }
        writeTcrdbSnapshot(out, database);
    }

    std::error_code error;
    std::filesystem::rename(tempPath, targetPath, error);
    if (error) {
        std::filesystem::remove(targetPath, error);
        error.clear();
        std::filesystem::rename(tempPath, targetPath, error);
    }
    if (error) {
        std::filesystem::remove(tempPath);
        throw std::runtime_error("failed to publish database snapshot");
    }
}

std::shared_ptr<Database> StorageManager::loadDatabase(std::string_view databaseName) const {
    std::ifstream in{tcrdbPathFor(root_, databaseName), std::ios::binary};
    if (!in) {
        throw std::runtime_error("failed to open database file for reading");
    }
    return readTcrdbSnapshot(in);
}

std::shared_ptr<Database> StorageManager::loadFirstDatabase() const {
    if (!std::filesystem::exists(root_)) {
        throw std::runtime_error("database storage directory does not exist");
    }
    for (const auto &entry : std::filesystem::directory_iterator{root_}) {
        if (entry.is_regular_file() && entry.path().extension() == kTcrdbExtension) {
            return loadDatabase(entry.path().stem().string());
        }
    }
    throw std::runtime_error("no saved database files found");
}

bool StorageManager::metadataExists(std::string_view databaseName) const {
    return std::filesystem::exists(tcrdbPathFor(root_, databaseName));
}

} // namespace VertexDB
