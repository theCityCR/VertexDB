#include "VertexDB/execution/recovery_service.hpp"

#include "VertexDB/execution/txn_session.hpp"
#include "VertexDB/parser/parser.hpp"

#include <cstddef>
#include <stdexcept>
#include <utility>
#include <vector>

namespace VertexDB {

RecoveryService::RecoveryService(StorageManager &storage, WriteAheadLog &wal, TxnSession &session,
                                 std::shared_ptr<Database> &database, ReplayQuery replayQuery)
    : storage_(storage), wal_(wal), session_(session), database_(database),
      replayQuery_(std::move(replayQuery)) {}

std::shared_ptr<Table> RecoveryService::requireTable(std::string_view tableName) const {
    if (!database_) {
        throw std::runtime_error("no active database");
    }
    auto table = database_->table(tableName);
    if (!table) {
        throw std::runtime_error("unknown table");
    }
    return table;
}

void RecoveryService::applyUndoRecord(const UndoRecord &record) {
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

void RecoveryService::recoverFromStorage() {
    bool loadedSnapshot = false;
    try {
        database_ = storage_.loadFirstDatabase();
        loadedSnapshot = true;
    } catch (const std::exception &) {
        database_.reset();
    }
    recoverFromWal(loadedSnapshot);
}

void RecoveryService::applyPhysicalRedo(const PhysicalRedoRecord &redo) {
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

void RecoveryService::applyPageImageRedo(const PageImageRedoRecord &redo) {
    auto table = requireTable(redo.tableName);
    std::vector<std::pair<PageId, std::vector<std::byte>>> heapPages;
    heapPages.reserve(redo.heapPages.size());
    for (const auto &page : redo.heapPages) {
        heapPages.emplace_back(page.pageId, page.bytes);
    }
    table->applyPageImageRedo(redo.hasHeapMeta, static_cast<std::size_t>(redo.capacity),
                              redo.freeList, std::move(heapPages), redo.btreeIndexes,
                              redo.hashIndexes);
}

void RecoveryService::recoverFromWal(bool loadedSnapshot) {
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
            if (record.operation == WalOperation::PageImageRedo) {
                for (const auto &redo : decodePageImageRedos(record.payload)) {
                    applyPageImageRedo(redo);
                }
                continue;
            }
            if (record.operation == WalOperation::CreateDatabase &&
                record.payload.find(' ') == std::string::npos) {
                database_ = std::make_shared<Database>(record.payload);
                continue;
            }
            // Legacy logical DML (Insert/Update/Delete) and DDL still replay as SQL.
            replayQuery_(parser.parse(record.payload));
        }
    } catch (const std::exception &) {
        database_.reset();
    }
    replayingWal_ = false;
}

void RecoveryService::appendWal(WalOperation operation, std::string payload) {
    if (replayingWal_) {
        return;
    }
    if (session_.transactionActive()) {
        session_.pushPendingWal(PendingWalRecord{operation, std::move(payload)});
        return;
    }
    (void)wal_.append(operation, std::move(payload));
}

void RecoveryService::appendPageImageRedo(Table &table, std::string tableName) {
    auto capture = table.takePageImageCapture();
    PageImageRedoRecord record;
    record.tableName = std::move(tableName);
    record.hasHeapMeta = true;
    record.capacity = capture.capacity;
    record.freeList = std::move(capture.freeList);
    record.heapPages.reserve(capture.heapPages.size());
    for (auto &[pageId, bytes] : capture.heapPages) {
        record.heapPages.push_back(HeapPageImage{pageId, std::move(bytes)});
    }
    record.btreeIndexes = std::move(capture.btreeIndexes);
    record.hashIndexes = std::move(capture.hashIndexes);
    appendWal(WalOperation::PageImageRedo, encodePageImageRedo(record));
}

void RecoveryService::flushPendingWal() {
    auto &pendingWal = session_.pendingWal();
    if (pendingWal.empty()) {
        return;
    }

    // Collapse a transaction's page-image (and legacy physical) redo records into one WAL append
    // per redo kind so a torn write cannot partially durable-commit the transaction.
    std::vector<PageImageRedoRecord> pageBatch;
    std::vector<PhysicalRedoRecord> physicalBatch;
    for (auto &record : pendingWal) {
        if (record.operation == WalOperation::PageImageRedo) {
            for (auto &redo : decodePageImageRedos(record.payload)) {
                pageBatch.push_back(std::move(redo));
            }
            continue;
        }
        if (record.operation == WalOperation::PhysicalRedo) {
            for (auto &redo : decodePhysicalRedos(record.payload)) {
                physicalBatch.push_back(std::move(redo));
            }
            continue;
        }
        (void)wal_.append(record.operation, std::move(record.payload));
    }
    if (!pageBatch.empty()) {
        (void)wal_.append(WalOperation::PageImageRedo, encodePageImageRedos(pageBatch));
    }
    if (!physicalBatch.empty()) {
        (void)wal_.append(WalOperation::PhysicalRedo, encodePhysicalRedos(physicalBatch));
    }
    session_.clearPendingWal();
}

void RecoveryService::clearPendingWal() noexcept { session_.clearPendingWal(); }

} // namespace VertexDB
