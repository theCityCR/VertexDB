#include "VertexDB/execution/txn_session.hpp"

#include "VertexDB/execution/select_helpers.hpp"
#include "VertexDB/persistence/page_image_redo.hpp"

#include <utility>

namespace VertexDB {

QueryResult TxnSession::begin() {
    if (transactionActive()) {
        return messageResult(false, "transaction already active");
    }
    activeTransaction_ = transactionManager_.begin().id;
    activeSnapshot_ = transactionManager_.currentSnapshot(*activeTransaction_);
    undoLog_.clear();
    clearPendingWal();
    return messageResult(true, "began transaction");
}

QueryResult TxnSession::commit() {
    if (!transactionActive()) {
        return messageResult(false, "no active transaction");
    }
    if (!transactionManager_.isSerializable(*activeTransaction_)) {
        return messageResult(false, "serialization failure");
    }
    transactionManager_.commit(*activeTransaction_);
    activeTransaction_.reset();
    activeSnapshot_.reset();
    undoLog_.clear();
    return messageResult(true, "committed transaction");
}

QueryResult TxnSession::rollback() {
    if (!transactionActive()) {
        return messageResult(false, "no active transaction");
    }
    transactionManager_.rollback(*activeTransaction_);
    activeTransaction_.reset();
    activeSnapshot_.reset();
    undoLog_.clear();
    clearPendingWal();
    return messageResult(true, "rolled back transaction");
}

void TxnSession::reset() noexcept {
    undoLog_.clear();
    clearPendingWal();
    activeTransaction_.reset();
    activeSnapshot_.reset();
}

ReadSnapshot TxnSession::readSnapshot() const {
    if (activeSnapshot_) {
        return *activeSnapshot_;
    }
    return transactionManager_.currentSnapshot();
}

TransactionId TxnSession::writeTransactionId() {
    if (activeTransaction_) {
        return *activeTransaction_;
    }
    return transactionManager_.beginCommitted();
}

bool TxnSession::transactionActive() const noexcept { return activeTransaction_.has_value(); }

std::optional<TransactionId> TxnSession::activeTransactionId() const noexcept {
    return activeTransaction_;
}

QueryResult TxnSession::rejectIfTransactionActive(std::string_view action) const {
    if (!transactionActive()) {
        return messageResult(true, {});
    }
    return messageResult(false, std::string(action) + " is not allowed while a transaction is active");
}

TransactionManager &TxnSession::transactionManager() noexcept { return transactionManager_; }

const TransactionManager &TxnSession::transactionManager() const noexcept {
    return transactionManager_;
}

UndoLog &TxnSession::undoLog() noexcept { return undoLog_; }

const UndoLog &TxnSession::undoLog() const noexcept { return undoLog_; }

void TxnSession::pushUndo(UndoRecord record) { undoLog_.push(std::move(record)); }

void TxnSession::pushPendingWal(PendingWalRecord record) {
    pendingWal_.push_back(std::move(record));
}

std::vector<PendingWalRecord> &TxnSession::pendingWal() noexcept { return pendingWal_; }

const std::vector<PendingWalRecord> &TxnSession::pendingWal() const noexcept {
    return pendingWal_;
}

void TxnSession::clearPendingWal() noexcept { pendingWal_.clear(); }

void TxnSession::rewriteTableName(std::string_view oldName, std::string_view newName) {
    undoLog_.rewriteTableName(oldName, newName);
    const std::string from{oldName};
    const std::string to{newName};
    for (auto &record : pendingWal_) {
        switch (record.operation) {
        case WalOperation::PageImageRedo: {
            auto redos = decodePageImageRedos(record.payload);
            bool changed = false;
            for (auto &redo : redos) {
                if (redo.tableName == from) {
                    redo.tableName = to;
                    changed = true;
                }
            }
            if (changed) {
                record.payload = encodePageImageRedos(redos);
            }
            break;
        }
        case WalOperation::CreateTable: {
            const std::string needle = "CREATE TABLE " + from + " (";
            const std::string replacement = "CREATE TABLE " + to + " (";
            if (record.payload.starts_with(needle)) {
                record.payload.replace(0, needle.size(), replacement);
            }
            break;
        }
        case WalOperation::DropTable: {
            const std::string needle = "DROP TABLE " + from + ";";
            if (record.payload == needle) {
                record.payload = "DROP TABLE " + to + ";";
            }
            break;
        }
        case WalOperation::CreateIndex: {
            const std::string needle = " ON " + from + "(";
            const std::string replacement = " ON " + to + "(";
            const auto pos = record.payload.find(needle);
            if (pos != std::string::npos) {
                record.payload.replace(pos, needle.size(), replacement);
            }
            break;
        }
        case WalOperation::DropIndex: {
            const std::string needle = " ON " + from + ";";
            const std::string replacement = " ON " + to + ";";
            const auto pos = record.payload.find(needle);
            if (pos != std::string::npos) {
                record.payload.replace(pos, needle.size(), replacement);
            }
            break;
        }
        case WalOperation::RenameTable: {
            // RENAME TABLE old TO new; — rewrite either side when it matches.
            const std::string prefix = "RENAME TABLE ";
            if (!record.payload.starts_with(prefix)) {
                break;
            }
            const auto toPos = record.payload.find(" TO ");
            if (toPos == std::string::npos || !record.payload.ends_with(";")) {
                break;
            }
            auto source = record.payload.substr(prefix.size(), toPos - prefix.size());
            auto dest = record.payload.substr(toPos + 4, record.payload.size() - (toPos + 4) - 1);
            if (source == from) {
                source = to;
            }
            if (dest == from) {
                dest = to;
            }
            record.payload = prefix + source + " TO " + dest + ";";
            break;
        }
        default:
            break;
        }
    }
}

} // namespace VertexDB
