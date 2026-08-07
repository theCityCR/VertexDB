#include "VertexDB/execution/txn_session.hpp"

#include <utility>

namespace VertexDB {
namespace {

QueryResult messageResult(bool success, std::string message) {
    QueryResult result;
    result.success = success;
    result.message = std::move(message);
    return result;
}

} // namespace

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

} // namespace VertexDB
