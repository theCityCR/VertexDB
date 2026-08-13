#include "VertexDB/persistence/tcrdb_codec.hpp"

#include "VertexDB/common/index_expression.hpp"
#include "VertexDB/parser/parser.hpp"
#include "VertexDB/storage/check_eval.hpp"
#include "tcrdb_detail.hpp"

#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

namespace VertexDB {
namespace {

std::filesystem::path pathFor(const std::filesystem::path &root, std::string_view databaseName) {
    return root / (std::string{databaseName} + std::string{kTcrdbExtension});
}

std::filesystem::path temporaryPathFor(const std::filesystem::path &root,
                                       std::string_view databaseName) {
    return root / (std::string{databaseName} + std::string{kTcrdbExtension} + ".tmp");
}

} // namespace

std::filesystem::path tcrdbPathFor(const std::filesystem::path &root, std::string_view databaseName) {
    return pathFor(root, databaseName);
}

std::filesystem::path tcrdbTemporaryPathFor(const std::filesystem::path &root,
                                            std::string_view databaseName) {
    return temporaryPathFor(root, databaseName);
}

void writeTcrdbSnapshot(std::ostream &out, const Database &database) {
    using namespace tcrdb_detail;

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
            writePodDb(out, static_cast<std::uint8_t>(column.unique ? 1 : 0));
            writePodDb(out, static_cast<std::uint8_t>(column.primaryKey ? 1 : 0));
        }

        writePodDb(out, static_cast<std::uint64_t>(table->checkConstraints().size()));
        for (const auto &check : table->checkConstraints()) {
            writeString(out, checkConstraintLiteral(check));
        }

        const auto indexes = table->indexDefinitions();
        writePodDb(out, static_cast<std::uint64_t>(indexes.size()));
        for (const auto &definition : indexes) {
            writeString(out, definition.name);
            writeString(out, encodeIndexDefinitionColumn(definition.column, definition.expression));
        }

        writePagePayloadRows(out, *table);
        writeIndexPages(out, *table);
        writeColumnHistograms(out, *table);
    }
}

std::shared_ptr<Database> readTcrdbSnapshot(std::istream &in) {
    using namespace tcrdb_detail;

    std::string magic(kMagic.size(), '\0');
    readBytesDb(in, magic.data(), magic.size());
    if (magic != kMagic) {
        throw std::runtime_error("invalid database file magic");
    }
    const auto version = readPodDb<std::uint32_t>(in);
    if (version != kVersion && version != kVersionV5 && version != kVersionV4 &&
        version != kVersionV3 && version != kVersionV2 && version != kVersionV1) {
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
            bool unique = false;
            bool primaryKey = false;
            if (version >= kVersionV5) {
                unique = readPodDb<std::uint8_t>(in) != 0;
                primaryKey = readPodDb<std::uint8_t>(in) != 0;
            }
            schema.push_back({std::move(columnName), type, nullable, unique, primaryKey});
        }

        std::vector<Predicate> checkConstraints;
        if (version >= kVersion) {
            const auto checkCount = readPodDb<std::uint64_t>(in);
            checkConstraints.reserve(static_cast<std::size_t>(checkCount));
            Parser parser;
            for (std::uint64_t checkIndex = 0; checkIndex < checkCount; ++checkIndex) {
                checkConstraints.push_back(
                    parser.parseCheckConstraintExpression(readString(in)));
            }
        }

        const bool created =
            database->createTable(tableName, std::move(schema), std::move(checkConstraints));
        if (!created) {
            throw std::runtime_error("duplicate table in database file");
        }
        auto table = database->table(tableName);

        const auto indexCount = readPodDb<std::uint64_t>(in);
        std::vector<std::pair<std::string, std::string>> indexDefinitions;
        indexDefinitions.reserve(static_cast<std::size_t>(indexCount));
        for (std::uint64_t index = 0; index < indexCount; ++index) {
            auto indexName = readString(in);
            auto columnName = readString(in);
            indexDefinitions.emplace_back(std::move(indexName), std::move(columnName));
        }

        const bool restoreIndexPages =
            version == kVersion || version == kVersionV5 || version == kVersionV4;
        for (const auto &definition : indexDefinitions) {
            const auto &indexName = definition.first;
            const auto decoded = decodeIndexDefinitionColumn(definition.second);
            const bool ok = [&]() {
                if (decoded.second) {
                    return restoreIndexPages
                               ? table->createIndexWithoutRebuild(indexName, *decoded.second)
                               : table->createIndex(indexName, *decoded.second);
                }
                return restoreIndexPages ? table->createIndexWithoutRebuild(indexName, decoded.first)
                                         : table->createIndex(indexName, decoded.first);
            }();
            if (!ok) {
                throw std::runtime_error("failed to restore index '" + indexName +
                                         "' on column '" + definition.second + "' for table '" +
                                         tableName + "'");
            }
        }

        if (version == kVersionV1) {
            loadDenseRows(*table, in, static_cast<std::size_t>(columnCount));
        } else if (version == kVersionV2) {
            loadSparseRows(*table, in, static_cast<std::size_t>(columnCount));
        } else {
            loadPagePayloadRows(*table, in, !restoreIndexPages);
            if (restoreIndexPages) {
                loadIndexPages(*table, in);
                (void)tryLoadColumnHistograms(*table, in);
            }
        }
        // After rows (and any restored index pages) so missing constraint indexes rebuild correctly.
        table->ensureConstraintIndexes();
    }

    return database;
}

} // namespace VertexDB
