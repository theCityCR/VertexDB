#include "VertexDB/execution/query_executor.hpp"

#include "VertexDB/parser/parser.hpp"
#include "VertexDB/persistence/physical_redo.hpp"

#include <stdexcept>

namespace VertexDB {

void QueryExecutor::applyUndoRecord(const UndoRecord &record) {
    auto table = database_->table(record.tableName);
    if (!table) {
        throw std::runtime_error("undo references unknown table " + record.tableName);
    }
    switch (record.kind) {
    case UndoKind::Insert:
        if (!table->eraseDiscardingVersion(record.rowId)) {
            throw std::runtime_error("failed to undo insert");
        }
        break;
    case UndoKind::Update:
        if (!record.beforeImage || !table->replaceRow(record.rowId, *record.beforeImage)) {
            throw std::runtime_error("failed to undo update");
        }
        break;
    case UndoKind::Delete:
        if (!record.beforeImage || !table->revive(record.rowId, *record.beforeImage)) {
            throw std::runtime_error("failed to undo delete");
        }
        break;
    }
}

void QueryExecutor::recoverFromStorage() {
    bool loadedSnapshot = false;
    try {
        database_ = storageManager_.loadFirstDatabase();
        loadedSnapshot = true;
    } catch (const std::exception &) {
        database_.reset();
    }
    recoverFromWal(loadedSnapshot);
}

void QueryExecutor::applyPhysicalRedo(const PhysicalRedoRecord &redo) {
    auto table = requireTable(redo.tableName);
    switch (redo.kind) {
    case PhysicalRedoKind::Upsert:
        if (!table->applyPhysicalUpsert(redo.rowId, redo.row)) {
            throw std::runtime_error("failed to apply physical upsert redo");
        }
        break;
    case PhysicalRedoKind::Erase:
        if (!table->applyPhysicalErase(redo.rowId)) {
            throw std::runtime_error("failed to apply physical erase redo");
        }
        break;
    }
}

void QueryExecutor::recoverFromWal(bool loadedSnapshot) {
    const auto records = wal_.readAll();
    if (records.empty()) {
        return;
    }

    std::size_t start = 0;
    if (loadedSnapshot) {
        for (std::size_t i = 0; i < records.size(); ++i) {
            if (records[i].operation == WalOperation::SaveDatabase) {
                start = i + 1;
            }
        }
    }

    replayingWal_ = true;
    try {
        Parser parser;
        for (std::size_t i = start; i < records.size(); ++i) {
            const auto &record = records[i];
            if (record.operation == WalOperation::SaveDatabase) {
                continue;
            }
            if (record.operation == WalOperation::PhysicalRedo) {
                for (const auto &redo : decodePhysicalRedos(record.payload)) {
                    applyPhysicalRedo(redo);
                }
                continue;
            }
            if (record.operation == WalOperation::CreateDatabase &&
                record.payload.find(' ') == std::string::npos) {
                database_ = std::make_shared<Database>(record.payload);
                continue;
            }
            // Legacy logical DML (Insert/Update/Delete) and DDL still replay as SQL.
            (void)executeUnlocked(parser.parse(record.payload));
        }
    } catch (const std::exception &) {
        database_.reset();
    }
    replayingWal_ = false;
}

void QueryExecutor::appendWal(WalOperation operation, std::string payload) {
    if (replayingWal_) {
        return;
    }
    if (transactionActive()) {
        pendingWal_.push_back(PendingWalRecord{operation, std::move(payload)});
        return;
    }
    (void)wal_.append(operation, std::move(payload));
}

void QueryExecutor::flushPendingWal() {
    if (pendingWal_.empty()) {
        return;
    }

    // Collapse a transaction's physical redo records into one WAL append so a torn write cannot
    // partially durable-commit the transaction.
    std::vector<PhysicalRedoRecord> batch;
    for (auto &record : pendingWal_) {
        if (record.operation != WalOperation::PhysicalRedo) {
            (void)wal_.append(record.operation, std::move(record.payload));
            continue;
        }
        for (auto &redo : decodePhysicalRedos(record.payload)) {
            batch.push_back(std::move(redo));
        }
    }
    if (!batch.empty()) {
        (void)wal_.append(WalOperation::PhysicalRedo, encodePhysicalRedos(batch));
    }
    pendingWal_.clear();
}

void QueryExecutor::clearPendingWal() noexcept { pendingWal_.clear(); }

} // namespace VertexDB
