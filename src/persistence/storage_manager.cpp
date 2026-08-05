#include "VertexDB/persistence/storage_manager.hpp"

#include <cstdint>
#include <fstream>
#include <stdexcept>

namespace VertexDB {
namespace {

constexpr std::string_view kMagic = "TCRDB001";
constexpr std::uint32_t kVersion = 1;
constexpr std::uint8_t kNullValueType = 255;
constexpr std::string_view kExtension = ".tcrdb";

void writeBytes(std::ostream &out, const void *data, std::size_t size) {
    out.write(static_cast<const char *>(data), static_cast<std::streamsize>(size));
    if (!out) {
        throw std::runtime_error("failed to write database file");
    }
}

void readBytes(std::istream &in, void *data, std::size_t size) {
    in.read(static_cast<char *>(data), static_cast<std::streamsize>(size));
    if (!in) {
        throw std::runtime_error("failed to read database file");
    }
}

template <typename T> void writePod(std::ostream &out, const T &value) {
    writeBytes(out, &value, sizeof(T));
}

template <typename T> T readPod(std::istream &in) {
    T value{};
    readBytes(in, &value, sizeof(T));
    return value;
}

void writeString(std::ostream &out, std::string_view value) {
    const auto size = static_cast<std::uint64_t>(value.size());
    writePod(out, size);
    writeBytes(out, value.data(), value.size());
}

std::string readString(std::istream &in) {
    const auto size = readPod<std::uint64_t>(in);
    std::string value(size, '\0');
    readBytes(in, value.data(), value.size());
    return value;
}

void writeValue(std::ostream &out, const Value &value) {
    if (value.isNull()) {
        writePod(out, kNullValueType);
        return;
    }
    const auto type = static_cast<std::uint8_t>(value.type());
    writePod(out, type);
    switch (value.type()) {
    case ColumnType::Int:
        writePod(out, std::get<std::int64_t>(value.data()));
        break;
    case ColumnType::Double:
        writePod(out, std::get<double>(value.data()));
        break;
    case ColumnType::String:
        writeString(out, std::get<std::string>(value.data()));
        break;
    }
}

Value readValue(std::istream &in) {
    const auto encodedType = readPod<std::uint8_t>(in);
    if (encodedType == kNullValueType) {
        return Value{};
    }
    const auto type = static_cast<ColumnType>(encodedType);
    switch (type) {
    case ColumnType::Int:
        return Value{readPod<std::int64_t>(in)};
    case ColumnType::Double:
        return Value{readPod<double>(in)};
    case ColumnType::String:
        return Value{readString(in)};
    }
    throw std::runtime_error("unsupported value type in database file");
}

std::filesystem::path pathFor(const std::filesystem::path &root, std::string_view databaseName) {
    return root / (std::string{databaseName} + std::string{kExtension});
}

std::filesystem::path temporaryPathFor(const std::filesystem::path &root,
                                       std::string_view databaseName) {
    return root / (std::string{databaseName} + std::string{kExtension} + ".tmp");
}

} // namespace

StorageManager::StorageManager(std::filesystem::path root) : root_(std::move(root)) {}

void StorageManager::saveDatabase(const Database &database) const {
    std::filesystem::create_directories(root_);
    const auto targetPath = pathFor(root_, database.name());
    const auto tempPath = temporaryPathFor(root_, database.name());
    {
        std::ofstream out{tempPath, std::ios::binary | std::ios::trunc};
        if (!out) {
            throw std::runtime_error("failed to open temporary database file for writing");
        }

        writeBytes(out, kMagic.data(), kMagic.size());
        writePod(out, kVersion);
        writeString(out, database.name());

        const auto tables = database.tables();
        writePod(out, static_cast<std::uint64_t>(tables.size()));
        for (const auto &table : tables) {
            writeString(out, table->name());

            writePod(out, static_cast<std::uint64_t>(table->schema().size()));
            for (const auto &column : table->schema()) {
                writeString(out, column.name);
                writePod(out, static_cast<std::uint8_t>(column.type));
                writePod(out, static_cast<std::uint8_t>(column.nullable ? 1 : 0));
            }

            const auto indexes = table->indexDefinitions();
            writePod(out, static_cast<std::uint64_t>(indexes.size()));
            for (const auto &[indexName, columnName] : indexes) {
                writeString(out, indexName);
                writeString(out, columnName);
            }

            const auto rows = table->rowsSnapshot();
            writePod(out, static_cast<std::uint64_t>(rows.size()));
            for (const auto &row : rows) {
                for (const auto &value : row) {
                    writeValue(out, value);
                }
            }
        }
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
    std::ifstream in{pathFor(root_, databaseName), std::ios::binary};
    if (!in) {
        throw std::runtime_error("failed to open database file for reading");
    }

    std::string magic(kMagic.size(), '\0');
    readBytes(in, magic.data(), magic.size());
    if (magic != kMagic) {
        throw std::runtime_error("invalid database file magic");
    }
    const auto version = readPod<std::uint32_t>(in);
    if (version != kVersion) {
        throw std::runtime_error("unsupported database file version");
    }

    auto database = std::make_shared<Database>(readString(in));
    const auto tableCount = readPod<std::uint64_t>(in);
    for (std::uint64_t tableIndex = 0; tableIndex < tableCount; ++tableIndex) {
        auto tableName = readString(in);
        const auto columnCount = readPod<std::uint64_t>(in);
        std::vector<Column> schema;
        schema.reserve(static_cast<std::size_t>(columnCount));
        for (std::uint64_t columnIndex = 0; columnIndex < columnCount; ++columnIndex) {
            auto columnName = readString(in);
            const auto type = static_cast<ColumnType>(readPod<std::uint8_t>(in));
            const bool nullable = readPod<std::uint8_t>(in) != 0;
            schema.push_back({std::move(columnName), type, nullable});
        }
        const bool created = database->createTable(tableName, std::move(schema));
        if (!created) {
            throw std::runtime_error("duplicate table in database file");
        }
        auto table = database->table(tableName);

        const auto indexCount = readPod<std::uint64_t>(in);
        std::vector<std::pair<std::string, std::string>> indexDefinitions;
        indexDefinitions.reserve(static_cast<std::size_t>(indexCount));
        for (std::uint64_t index = 0; index < indexCount; ++index) {
            indexDefinitions.emplace_back(readString(in), readString(in));
        }

        const auto rowCount = readPod<std::uint64_t>(in);
        std::vector<Row> rows;
        rows.reserve(static_cast<std::size_t>(rowCount));
        for (std::uint64_t rowIndex = 0; rowIndex < rowCount; ++rowIndex) {
            Row row;
            row.reserve(static_cast<std::size_t>(columnCount));
            for (std::uint64_t columnIndex = 0; columnIndex < columnCount; ++columnIndex) {
                row.push_back(readValue(in));
            }
            rows.push_back(std::move(row));
        }
        table->replaceRows(std::move(rows));
        for (const auto &[indexName, columnName] : indexDefinitions) {
            table->createIndex(indexName, columnName);
        }
    }

    return database;
}

std::shared_ptr<Database> StorageManager::loadFirstDatabase() const {
    if (!std::filesystem::exists(root_)) {
        throw std::runtime_error("database storage directory does not exist");
    }
    for (const auto &entry : std::filesystem::directory_iterator{root_}) {
        if (entry.is_regular_file() && entry.path().extension() == kExtension) {
            return loadDatabase(entry.path().stem().string());
        }
    }
    throw std::runtime_error("no saved database files found");
}

bool StorageManager::metadataExists(std::string_view databaseName) const {
    return std::filesystem::exists(pathFor(root_, databaseName));
}

} // namespace VertexDB
