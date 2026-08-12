#include "VertexDB/transaction/transaction_manager.hpp"

#include <algorithm>
#include <string>

namespace VertexDB {

namespace {

bool intersects(const std::unordered_set<ConflictKey> &left,
                const std::unordered_set<ConflictKey> &right) {
    if (left.empty() || right.empty()) {
        return false;
    }
    const auto &smaller = left.size() <= right.size() ? left : right;
    const auto &larger = left.size() <= right.size() ? right : left;
    for (const auto key : smaller) {
        if (larger.contains(key)) {
            return true;
        }
    }
    return false;
}

} // namespace

ConflictKey TransactionManager::conflictKey(std::string_view relation, std::size_t rowId) {
    ConflictKey key;
    key.reserve(relation.size() + 24);
    key.append(relation);
    key.push_back('\0');
    key.append(std::to_string(rowId));
    return key;
}

Transaction TransactionManager::begin() {
    Transaction transaction{nextId_++, TransactionState::Active, std::nullopt, nextCommitSeq_};
    transactions_.emplace(transaction.id, transaction);
    activeReads_.emplace(transaction.id, std::unordered_set<ConflictKey>{});
    activeWrites_.emplace(transaction.id, std::unordered_set<ConflictKey>{});
    return transaction;
}

bool TransactionManager::isSerializable(TransactionId id) const {
    const auto it = transactions_.find(id);
    if (it == transactions_.end() || it->second.state != TransactionState::Active) {
        return false;
    }
    const auto readIt = activeReads_.find(id);
    const auto writeIt = activeWrites_.find(id);
    const auto &reads =
        readIt == activeReads_.end() ? std::unordered_set<ConflictKey>{} : readIt->second;
    const auto &writes =
        writeIt == activeWrites_.end() ? std::unordered_set<ConflictKey>{} : writeIt->second;
    for (const auto &[seq, committedWrites] : committedWriteSets_) {
        if (seq <= it->second.snapshotMaxCommitSeq) {
            continue;
        }
        if (intersects(reads, committedWrites) || intersects(writes, committedWrites)) {
            return false;
        }
    }
    return true;
}

void TransactionManager::commit(TransactionId id) {
    auto it = transactions_.find(id);
    if (it == transactions_.end() || it->second.state != TransactionState::Active) {
        throw std::runtime_error("cannot commit inactive transaction");
    }
    if (!isSerializable(id)) {
        rollback(id);
        throw SerializationFailure{};
    }
    it->second.state = TransactionState::Committed;
    it->second.commitSeq = ++nextCommitSeq_;
    auto writeIt = activeWrites_.find(id);
    if (writeIt != activeWrites_.end() && !writeIt->second.empty()) {
        committedWriteSets_.emplace(*it->second.commitSeq, std::move(writeIt->second));
    }
    activeWrites_.erase(id);
    activeReads_.erase(id);
    pruneCommittedWriteSets();
}

void TransactionManager::rollback(TransactionId id) {
    auto it = transactions_.find(id);
    if (it == transactions_.end() || it->second.state != TransactionState::Active) {
        throw std::runtime_error("cannot roll back inactive transaction");
    }
    it->second.state = TransactionState::RolledBack;
    activeReads_.erase(id);
    activeWrites_.erase(id);
    pruneCommittedWriteSets();
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

void TransactionManager::recordRead(TransactionId id, std::string_view relation,
                                     std::size_t rowId) {
    if (id == kSystemTransactionId) {
        return;
    }
    auto it = activeReads_.find(id);
    if (it == activeReads_.end()) {
        return;
    }
    it->second.insert(conflictKey(relation, rowId));
}

void TransactionManager::recordWrite(TransactionId id, std::string_view relation,
                                      std::size_t rowId) {
    if (id == kSystemTransactionId) {
        return;
    }
    const auto key = conflictKey(relation, rowId);
    auto activeIt = activeWrites_.find(id);
    if (activeIt != activeWrites_.end()) {
        activeIt->second.insert(key);
        return;
    }
    // Autocommit: beginCommitted() finishes before DML stamps the row; attach to that commit.
    const auto txn = find(id);
    if (!txn || txn->state != TransactionState::Committed || !txn->commitSeq) {
        return;
    }
    committedWriteSets_[*txn->commitSeq].insert(key);
}

void TransactionManager::pruneCommittedWriteSets() {
    if (activeReads_.empty()) {
        committedWriteSets_.clear();
        return;
    }
    CommitSeq minSnapshot = nextCommitSeq_;
    for (const auto &[id, _] : activeReads_) {
        const auto txn = find(id);
        if (txn && txn->state == TransactionState::Active) {
            minSnapshot = std::min(minSnapshot, txn->snapshotMaxCommitSeq);
        }
    }
    for (auto it = committedWriteSets_.begin(); it != committedWriteSets_.end();) {
        if (it->first <= minSnapshot) {
            it = committedWriteSets_.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace VertexDB
