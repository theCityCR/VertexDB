#include "VertexDB/transaction/transaction_manager.hpp"

#include <stdexcept>

namespace VertexDB {

Transaction TransactionManager::begin() {
    Transaction transaction{nextId_++, TransactionState::Active, std::nullopt};
    transactions_.emplace(transaction.id, transaction);
    return transaction;
}

void TransactionManager::commit(TransactionId id) {
    auto it = transactions_.find(id);
    if (it == transactions_.end() || it->second.state != TransactionState::Active) {
        throw std::runtime_error("cannot commit inactive transaction");
    }
    it->second.state = TransactionState::Committed;
    it->second.commitSeq = ++nextCommitSeq_;
}

void TransactionManager::rollback(TransactionId id) {
    auto it = transactions_.find(id);
    if (it == transactions_.end() || it->second.state != TransactionState::Active) {
        throw std::runtime_error("cannot roll back inactive transaction");
    }
    it->second.state = TransactionState::RolledBack;
}

TransactionId TransactionManager::beginCommitted() {
    const auto transaction = begin();
    commit(transaction.id);
    return transaction.id;
}

std::optional<Transaction> TransactionManager::find(TransactionId id) const {
    auto it = transactions_.find(id);
    if (it == transactions_.end()) {
        return std::nullopt;
    }
    return it->second;
}

CommitSeq TransactionManager::currentCommitSeq() const noexcept { return nextCommitSeq_; }

ReadSnapshot TransactionManager::currentSnapshot(TransactionId self) const {
    return ReadSnapshot{self, nextCommitSeq_};
}

bool TransactionManager::isCreatorVisible(TransactionId creator,
                                           const ReadSnapshot &snapshot) const {
    if (creator == kSystemTransactionId) {
        return true;
    }
    if (creator == snapshot.self) {
        return true;
    }
    const auto transaction = find(creator);
    if (!transaction || transaction->state != TransactionState::Committed ||
        !transaction->commitSeq) {
        return false;
    }
    return *transaction->commitSeq <= snapshot.maxCommitSeq;
}

bool TransactionManager::isVisible(TransactionId createdBy,
                                   std::optional<TransactionId> deletedBy,
                                   const ReadSnapshot &snapshot) const {
    if (!isCreatorVisible(createdBy, snapshot)) {
        return false;
    }
    if (deletedBy && isCreatorVisible(*deletedBy, snapshot)) {
        return false;
    }
    return true;
}

} // namespace VertexDB
