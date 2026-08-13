#pragma once

// FOREIGN KEY enforcement helpers used by CatalogEngine / DmlEngine.
// Parent existence uses SI-visible rows (indexed lookup + visibleEntriesById).

#include "VertexDB/common/value.hpp"
#include "VertexDB/storage/database.hpp"
#include "VertexDB/storage/foreign_key.hpp"
#include "VertexDB/storage/row.hpp"
#include "VertexDB/storage/table.hpp"
#include "VertexDB/transaction/mvcc_row_store.hpp"

#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace VertexDB {

class TransactionManager;

// Validate FK definitions against the catalog. When `creatingSchema` is set and
// `childTableName` matches an FK parent table name, validate against that schema (self-FK).
void validateForeignKeyDefinitions(const Database &database, std::string_view childTableName,
                                   std::span<const Column> childSchema,
                                   std::span<const ForeignKeyConstraint> foreignKeys,
                                   std::span<const Column> creatingSchema = {});

// Reject INSERT/UPDATE child row images that reference a missing parent key.
void assertForeignKeysOnChildRow(Database &database, const Table &child, const Row &row,
                                 const ReadSnapshot &snapshot, TransactionManager &transactions);

// Reject DELETE (or UPDATE of a referenced parent key) when SI-visible children still reference it.
void assertParentKeyNotReferenced(Database &database, const Table &parent, const Row &parentRow,
                                  const ReadSnapshot &snapshot, TransactionManager &transactions,
                                  std::optional<std::size_t> updatingColumn = std::nullopt,
                                  const Value *newValue = nullptr);

[[nodiscard]] bool tableIsForeignKeyParent(const Database &database, std::string_view tableName);

[[nodiscard]] std::string foreignKeyLiteral(const ForeignKeyConstraint &fk);

} // namespace VertexDB
