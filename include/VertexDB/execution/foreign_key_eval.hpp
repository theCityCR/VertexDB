#pragma once

// FOREIGN KEY enforcement helpers used by CatalogEngine / DmlEngine.
// Parent existence uses SI-visible rows (indexed lookup + visibleEntriesById).

#include "VertexDB/common/value.hpp"
#include "VertexDB/storage/database.hpp"
#include "VertexDB/storage/foreign_key.hpp"
#include "VertexDB/storage/row.hpp"
#include "VertexDB/storage/table.hpp"
#include "VertexDB/storage/unique_constraint.hpp"
#include "VertexDB/transaction/mvcc_row_store.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace VertexDB {

class TransactionManager;

inline constexpr std::size_t kMaxReferentialActionDepth = 64;

struct ForeignKeyChildHit {
    std::shared_ptr<Table> table;
    std::string tableName;
    ForeignKeyConstraint fk;
    RowId rowId{};
    Row row;
};

// Validate FK definitions against the catalog. When `creatingSchema` is set and
// `childTableName` matches an FK parent table name, validate against that schema (self-FK).
// `creatingUniques` supplies table-level UNIQUE/PK of the table being created.
void validateForeignKeyDefinitions(const Database &database, std::string_view childTableName,
                                   std::span<const Column> childSchema,
                                   std::span<const ForeignKeyConstraint> foreignKeys,
                                   std::span<const Column> creatingSchema = {},
                                   std::span<const UniqueConstraint> creatingUniques = {});

// Reject INSERT/UPDATE child row images that reference a missing parent key.
void assertForeignKeysOnChildRow(Database &database, const Table &child, const Row &row,
                                 const ReadSnapshot &snapshot, TransactionManager &transactions);

// Collect SI-visible child rows that reference the parent key (for DELETE or parent-key UPDATE).
[[nodiscard]] std::vector<ForeignKeyChildHit>
collectReferencingChildren(Database &database, const Table &parent, const Row &parentRow,
                           const ReadSnapshot &snapshot, TransactionManager &transactions,
                           std::optional<std::size_t> updatingColumn = std::nullopt,
                           const Value *newValue = nullptr);

// Reject when any NO ACTION child still references the parent key.
void assertNoActionParentKeyNotReferenced(std::span<const ForeignKeyChildHit> refs,
                                          bool forUpdate);

[[nodiscard]] bool tableIsForeignKeyParent(const Database &database, std::string_view tableName);

[[nodiscard]] std::string foreignKeyLiteral(const ForeignKeyConstraint &fk);

} // namespace VertexDB
