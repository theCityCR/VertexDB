#include "VertexDB/persistence/storage_manager.hpp"

#include "VertexDB/common/binary_io.hpp"

#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace VertexDB {
namespace {

constexpr std::string_view kMagic = "TCRDB001";
constexpr std::uint32_t kVersionV1 = 1;
constexpr std::uint32_t kVersionV2 = 2;
constexpr std::uint32_t kVersionV3 = 3;
constexpr std::uint32_t kVersion = 4;
constexpr std::uint8_t kNullValueType = 255;
constexpr std::string_view kExtension = ".tcrdb";
constexpr std::string_view kWriteError = "failed to write database file";
constexpr std::string_view kReadError = "failed to read database file";

template <typename T> void writePodDb(std::ostream &out, const T &value) {
    writePod(out, value, kWriteError);
}

template <typename T> [[nodiscard]] T readPodDb(std::istream &in) {
    return readPod<T>(in, kReadError);
}

void writeBytesDb(std::ostream &out, const void *data, std::size_t size) {
    writeBytes(out, data, size, kWriteError);
}

void readBytesDb(std::istream &in, void *data, std::size_t size) {
    readBytes(in, data, size, kReadError);
}

void writeString(std::ostream &out, std::string_view value) {
    const auto size = static_cast<std::uint64_t>(value.size());
    writePodDb(out, size);
    writeBytesDb(out, value.data(), value.size());
}

std::string readString(std::istream &in) {
    const auto size = readPodDb<std::uint64_t>(in);
    std::string value(size, '\0');
    readBytesDb(in, value.data(), value.size());
    return value;
}

void writeValue(std::ostream &out, const Value &value) {
    if (value.isNull()) {
        writePodDb(out, kNullValueType);
        return;
    }
    writePodDb(out, static_cast<std::uint8_t>(value.type()));
    switch (value.type()) {
    case ColumnType::Int:
        writePodDb(out, std::get<std::int64_t>(value.data()));
        break;
    case ColumnType::Double:
        writePodDb(out, std::get<double>(value.data()));
        break;
    case ColumnType::String:
        writeString(out, std::get<std::string>(value.data()));
        break;
    }
}

Value readValue(std::istream &in) {
    const auto encodedType = readPodDb<std::uint8_t>(in);
    if (encodedType == kNullValueType) {
        return Value{};
    }
    const auto type = static_cast<ColumnType>(encodedType);
    switch (type) {
    case ColumnType::Int:
        return Value{readPodDb<std::int64_t>(in)};
    case ColumnType::Double:
        return Value{readPodDb<double>(in)};
    case ColumnType::String:
        return Value{readString(in)};
    }
    throw std::runtime_error("unsupported value type in database file");
}

Row readRow(std::istream &in, std::size_t columnCount) {
    Row row;
    row.reserve(columnCount);
    for (std::size_t columnIndex = 0; columnIndex < columnCount; ++columnIndex) {
        row.push_back(readValue(in));
    }
    return row;
}

std::filesystem::path pathFor(const std::filesystem::path &root, std::string_view databaseName) {
    return root / (std::string{databaseName} + std::string{kExtension});
}

std::filesystem::path temporaryPathFor(const std::filesystem::path &root,
                                       std::string_view databaseName) {
    return root / (std::string{databaseName} + std::string{kExtension} + ".tmp");
}

void loadDenseRows(Table &table, std::istream &in, std::size_t columnCount) {
    const auto rowCount = readPodDb<std::uint64_t>(in);
    std::vector<Row> rows;
    rows.reserve(static_cast<std::size_t>(rowCount));
    for (std::uint64_t rowIndex = 0; rowIndex < rowCount; ++rowIndex) {
        rows.push_back(readRow(in, columnCount));
    }
    table.replaceRows(std::move(rows));
}

void loadSparseRows(Table &table, std::istream &in, std::size_t columnCount) {
    const auto capacity = static_cast<std::size_t>(readPodDb<std::uint64_t>(in));
    const auto freeCount = readPodDb<std::uint64_t>(in);
    std::vector<RowId> freeList;
    freeList.reserve(static_cast<std::size_t>(freeCount));
    for (std::uint64_t index = 0; index < freeCount; ++index) {
        freeList.push_back(static_cast<RowId>(readPodDb<std::uint64_t>(in)));
    }

    const auto liveCount = readPodDb<std::uint64_t>(in);
    std::vector<std::pair<RowId, Row>> entries;
    entries.reserve(static_cast<std::size_t>(liveCount));
    for (std::uint64_t index = 0; index < liveCount; ++index) {
        const auto rowId = static_cast<RowId>(readPodDb<std::uint64_t>(in));
        entries.emplace_back(rowId, readRow(in, columnCount));
    }
    table.replaceSparse(capacity, std::move(freeList), std::move(entries));
}

void loadPagePayloadRows(Table &table, std::istream &in, bool rebuildIndexes) {
    PageStoreSnapshot snapshot;
    snapshot.rowsPerPage = static_cast<std::size_t>(readPodDb<std::uint64_t>(in));
    snapshot.capacity = static_cast<std::size_t>(readPodDb<std::uint64_t>(in));

    const auto freeCount = readPodDb<std::uint64_t>(in);
    snapshot.freeList.reserve(static_cast<std::size_t>(freeCount));
    for (std::uint64_t index = 0; index < freeCount; ++index) {
        snapshot.freeList.push_back(static_cast<RowId>(readPodDb<std::uint64_t>(in)));
    }

    const auto pageCount = readPodDb<std::uint64_t>(in);
    snapshot.pages.reserve(static_cast<std::size_t>(pageCount));
    for (std::uint64_t index = 0; index < pageCount; ++index) {
        const auto pageId = static_cast<PageId>(readPodDb<std::uint64_t>(in));
        const auto byteLength = static_cast<std::size_t>(readPodDb<std::uint64_t>(in));
        std::vector<std::byte> bytes(byteLength);
        if (byteLength > 0) {
            readBytesDb(in, bytes.data(), bytes.size());
        }
        snapshot.pages.emplace_back(pageId, std::move(bytes));
    }
    table.replaceFromPages(std::move(snapshot), rebuildIndexes);
}

void writePagePayloadRows(std::ostream &out, const Table &table) {
    const auto snapshot = table.exportPageStore();
    writePodDb(out, static_cast<std::uint64_t>(snapshot.rowsPerPage));
    writePodDb(out, static_cast<std::uint64_t>(snapshot.capacity));
    writePodDb(out, static_cast<std::uint64_t>(snapshot.freeList.size()));
    for (const auto rowId : snapshot.freeList) {
        writePodDb(out, static_cast<std::uint64_t>(rowId));
    }
    writePodDb(out, static_cast<std::uint64_t>(snapshot.pages.size()));
    for (const auto &[pageId, bytes] : snapshot.pages) {
        writePodDb(out, static_cast<std::uint64_t>(pageId));
        writePodDb(out, static_cast<std::uint64_t>(bytes.size()));
        if (!bytes.empty()) {
            writeBytesDb(out, bytes.data(), bytes.size());
        }
    }
}

void writeBTreeNode(std::ostream &out, const BTreeNode &node) {
    writePodDb(out, static_cast<std::uint64_t>(node.pageId));
    writePodDb(out, static_cast<std::uint8_t>(node.leaf ? 1 : 0));
    writePodDb(out, static_cast<std::uint64_t>(node.keys.size()));
    for (const auto &key : node.keys) {
        writeValue(out, key);
    }
    if (node.leaf) {
        writePodDb(out, static_cast<std::uint64_t>(node.rowIds.size()));
        for (const auto &rowIds : node.rowIds) {
            writePodDb(out, static_cast<std::uint64_t>(rowIds.size()));
            for (const auto rowId : rowIds) {
                writePodDb(out, static_cast<std::uint64_t>(rowId));
            }
        }
        writePodDb(out, static_cast<std::uint8_t>(node.nextLeaf.has_value() ? 1 : 0));
        if (node.nextLeaf) {
            writePodDb(out, static_cast<std::uint64_t>(*node.nextLeaf));
        }
    } else {
        writePodDb(out, static_cast<std::uint64_t>(node.children.size()));
        for (const auto child : node.children) {
            writePodDb(out, static_cast<std::uint64_t>(child));
        }
    }
}

BTreeNode readBTreeNode(std::istream &in) {
    BTreeNode node;
    node.pageId = readPodDb<std::uint64_t>(in);
    node.leaf = readPodDb<std::uint8_t>(in) != 0;
    const auto keyCount = readPodDb<std::uint64_t>(in);
    node.keys.reserve(static_cast<std::size_t>(keyCount));
    for (std::uint64_t i = 0; i < keyCount; ++i) {
        node.keys.push_back(readValue(in));
    }
    if (node.leaf) {
        const auto groupCount = readPodDb<std::uint64_t>(in);
        node.rowIds.reserve(static_cast<std::size_t>(groupCount));
        for (std::uint64_t i = 0; i < groupCount; ++i) {
            const auto rowCount = readPodDb<std::uint64_t>(in);
            std::vector<RowId> rowIds;
            rowIds.reserve(static_cast<std::size_t>(rowCount));
            for (std::uint64_t r = 0; r < rowCount; ++r) {
                rowIds.push_back(static_cast<RowId>(readPodDb<std::uint64_t>(in)));
            }
            node.rowIds.push_back(std::move(rowIds));
        }
        if (readPodDb<std::uint8_t>(in) != 0) {
            node.nextLeaf = readPodDb<std::uint64_t>(in);
        }
    } else {
        const auto childCount = readPodDb<std::uint64_t>(in);
        node.children.reserve(static_cast<std::size_t>(childCount));
        for (std::uint64_t i = 0; i < childCount; ++i) {
            node.children.push_back(readPodDb<std::uint64_t>(in));
        }
    }
    return node;
}

void writeBTreeSnapshot(std::ostream &out, const BTreeIndexSnapshot &snapshot) {
    writePodDb(out, static_cast<std::uint64_t>(snapshot.maxKeysPerNode));
    writePodDb(out, static_cast<std::uint64_t>(snapshot.rootPageId));
    writePodDb(out, static_cast<std::uint64_t>(snapshot.nextPageId));
    writePodDb(out, static_cast<std::uint64_t>(snapshot.keyCount));
    writePodDb(out, static_cast<std::uint64_t>(snapshot.freePageIds.size()));
    for (const auto id : snapshot.freePageIds) {
        writePodDb(out, static_cast<std::uint64_t>(id));
    }
    writePodDb(out, static_cast<std::uint64_t>(snapshot.nodes.size()));
    for (const auto &node : snapshot.nodes) {
        writeBTreeNode(out, node);
    }
}

BTreeIndexSnapshot readBTreeSnapshot(std::istream &in) {
    BTreeIndexSnapshot snapshot;
    snapshot.maxKeysPerNode = static_cast<std::size_t>(readPodDb<std::uint64_t>(in));
    snapshot.rootPageId = readPodDb<std::uint64_t>(in);
    snapshot.nextPageId = readPodDb<std::uint64_t>(in);
    snapshot.keyCount = static_cast<std::size_t>(readPodDb<std::uint64_t>(in));
    const auto freeCount = readPodDb<std::uint64_t>(in);
    snapshot.freePageIds.reserve(static_cast<std::size_t>(freeCount));
    for (std::uint64_t i = 0; i < freeCount; ++i) {
        snapshot.freePageIds.push_back(readPodDb<std::uint64_t>(in));
    }
    const auto nodeCount = readPodDb<std::uint64_t>(in);
    snapshot.nodes.reserve(static_cast<std::size_t>(nodeCount));
    for (std::uint64_t i = 0; i < nodeCount; ++i) {
        snapshot.nodes.push_back(readBTreeNode(in));
    }
    return snapshot;
}

void writeHashSnapshot(std::ostream &out, const HashIndexSnapshot &snapshot) {
    writePodDb(out, static_cast<std::uint64_t>(snapshot.buckets.size()));
    for (const auto &[key, rowIds] : snapshot.buckets) {
        writeValue(out, key);
        writePodDb(out, static_cast<std::uint64_t>(rowIds.size()));
        for (const auto rowId : rowIds) {
            writePodDb(out, static_cast<std::uint64_t>(rowId));
        }
    }
}

HashIndexSnapshot readHashSnapshot(std::istream &in) {
    HashIndexSnapshot snapshot;
    snapshot.replaceAll = true;
    const auto bucketCount = readPodDb<std::uint64_t>(in);
    snapshot.buckets.reserve(static_cast<std::size_t>(bucketCount));
    for (std::uint64_t i = 0; i < bucketCount; ++i) {
        auto key = readValue(in);
        const auto rowCount = readPodDb<std::uint64_t>(in);
        std::vector<RowId> rowIds;
        rowIds.reserve(static_cast<std::size_t>(rowCount));
        for (std::uint64_t r = 0; r < rowCount; ++r) {
            rowIds.push_back(static_cast<RowId>(readPodDb<std::uint64_t>(in)));
        }
        snapshot.buckets.emplace_back(std::move(key), std::move(rowIds));
    }
    return snapshot;
}

void writeIndexPages(std::ostream &out, const Table &table) {
    const auto snapshot = table.exportIndexPages();
    writePodDb(out, static_cast<std::uint64_t>(snapshot.indexes.size()));
    for (const auto &entry : snapshot.indexes) {
        writeString(out, entry.name);
        writeString(out, entry.column);
        writeBTreeSnapshot(out, entry.btree);
        writeHashSnapshot(out, entry.hash);
    }
}

void loadIndexPages(Table &table, std::istream &in) {
    TableIndexStoreSnapshot snapshot;
    const auto indexCount = readPodDb<std::uint64_t>(in);
    snapshot.indexes.reserve(static_cast<std::size_t>(indexCount));
    for (std::uint64_t i = 0; i < indexCount; ++i) {
        IndexStoreSnapshot entry;
        entry.name = readString(in);
        entry.column = readString(in);
        entry.btree = readBTreeSnapshot(in);
        entry.hash = readHashSnapshot(in);
        snapshot.indexes.push_back(std::move(entry));
    }
    table.replaceIndexPages(std::move(snapshot));
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

        writeBytesDb(out, kMagic.data(), kMagic.size());
        writePodDb(out, kVersion);
        writeString(out, database.name());

        const auto tables = database.tables();
        writePodDb(out, static_cast<std::uint64_t>(tables.size()));
        for (const auto &table : tables) {
            writeString(out, table->name());

            writePodDb(out, static_cast<std::uint64_t>(table->schema().size()));
            for (const auto &column : table->schema()) {
                writeString(out, column.name);
                writePodDb(out, static_cast<std::uint8_t>(column.type));
                writePodDb(out, static_cast<std::uint8_t>(column.nullable ? 1 : 0));
            }

            const auto indexes = table->indexDefinitions();
            writePodDb(out, static_cast<std::uint64_t>(indexes.size()));
            for (const auto &[indexName, columnName] : indexes) {
                writeString(out, indexName);
                writeString(out, columnName);
            }

            writePagePayloadRows(out, *table);
            writeIndexPages(out, *table);
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
    readBytesDb(in, magic.data(), magic.size());
    if (magic != kMagic) {
        throw std::runtime_error("invalid database file magic");
    }
    const auto version = readPodDb<std::uint32_t>(in);
    if (version != kVersion && version != kVersionV3 && version != kVersionV2 &&
        version != kVersionV1) {
        throw std::runtime_error("unsupported database file version");
    }

    auto database = std::make_shared<Database>(readString(in));
    const auto tableCount = readPodDb<std::uint64_t>(in);
    for (std::uint64_t tableIndex = 0; tableIndex < tableCount; ++tableIndex) {
        auto tableName = readString(in);
        const auto columnCount = readPodDb<std::uint64_t>(in);
        std::vector<Column> schema;
        schema.reserve(static_cast<std::size_t>(columnCount));
        for (std::uint64_t columnIndex = 0; columnIndex < columnCount; ++columnIndex) {
            auto columnName = readString(in);
            const auto type = static_cast<ColumnType>(readPodDb<std::uint8_t>(in));
            const bool nullable = readPodDb<std::uint8_t>(in) != 0;
            schema.push_back({std::move(columnName), type, nullable});
        }
        const bool created = database->createTable(tableName, std::move(schema));
        if (!created) {
            throw std::runtime_error("duplicate table in database file");
        }
        auto table = database->table(tableName);

        const auto indexCount = readPodDb<std::uint64_t>(in);
        std::vector<std::pair<std::string, std::string>> indexDefinitions;
        indexDefinitions.reserve(static_cast<std::size_t>(indexCount));
        for (std::uint64_t index = 0; index < indexCount; ++index) {
            // Read sequentially: argument evaluation order for emplace_back(read(), read())
            // is unspecified and libstdc++/MSVC evaluate right-to-left, swapping fields.
            auto indexName = readString(in);
            auto columnName = readString(in);
            indexDefinitions.emplace_back(std::move(indexName), std::move(columnName));
        }

        const bool restoreIndexPages = version == kVersion;
        for (const auto &definition : indexDefinitions) {
            const auto &indexName = definition.first;
            const auto &columnName = definition.second;
            const bool ok = restoreIndexPages
                                ? table->createIndexWithoutRebuild(indexName, columnName)
                                : table->createIndex(indexName, columnName);
            if (!ok) {
                throw std::runtime_error("failed to restore index '" + indexName +
                                         "' on column '" + columnName + "' for table '" +
                                         tableName + "'");
            }
        }

        if (version == kVersionV1) {
            loadDenseRows(*table, in, static_cast<std::size_t>(columnCount));
        } else if (version == kVersionV2) {
            loadSparseRows(*table, in, static_cast<std::size_t>(columnCount));
        } else {
            // v3 rebuilds indexes from rows; v4 restores index pages without rebuild.
            loadPagePayloadRows(*table, in, !restoreIndexPages);
            if (restoreIndexPages) {
                loadIndexPages(*table, in);
            }
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
