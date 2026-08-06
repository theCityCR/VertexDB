#include "VertexDB/execution/query_executor.hpp"

#include "VertexDB/parser/parser.hpp"

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

void QueryExecutor::recoverFromWal(bool loadedSnapshot) {
    std::vector<WalRecord> records;
    try {
        records = wal_.readAll();
    } catch (const std::exception &) {
        return;
    }
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
            if (record.operation == WalOperation::CreateDatabase &&
                record.payload.find(' ') == std::string::npos) {
                database_ = std::make_shared<Database>(record.payload);
                continue;
            }
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
    for (auto &record : pendingWal_) {
        (void)wal_.append(record.operation, std::move(record.payload));
    }
    pendingWal_.clear();
}

void QueryExecutor::clearPendingWal() noexcept { pendingWal_.clear(); }

} // namespace VertexDB
