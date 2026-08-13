#include "VertexDB/transaction/transaction_manager.hpp"

#include "VertexDB/common/string_pattern.hpp"

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
    for (const auto &key : smaller) {
        if (larger.contains(key)) {
            return true;
        }
    }
    return false;
}

bool compareCell(const Value &left, ComparisonOperator op, const Value &right) {
    switch (op) {
    case ComparisonOperator::Equal:
        return left == right;
    case ComparisonOperator::Greater:
        return right < left;
    case ComparisonOperator::Less:
        return left < right;
    }
    return false;
}

bool insertMatchesPredicate(const SsiInsert &insert, const SsiPredicate &predicate) {
    if (insert.relation != predicate.relation) {
        return false;
    }
    if (predicate.column.empty()) {
        return true; // relation-wide membership
    }
    for (const auto &[name, value] : insert.columns) {
        if (name != predicate.column) {
            continue;
        }
        if (predicate.likePattern) {
            if (value.isNull() || value.type() != ColumnType::String) {
                return false;
            }
            return matchLikePattern(std::get<std::string>(value.data()), *predicate.likePattern);
        }
        if (!predicate.op || !predicate.value) {
            return true; // column present but incomplete comparison → treat as membership
        }
        return compareCell(value, *predicate.op, *predicate.value);
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
    activePredicateReads_.emplace(transaction.id, std::vector<SsiPredicate>{});
    activeInserts_.emplace(transaction.id, std::vector<SsiInsert>{});
    return transaction;
}

bool TransactionManager::predicateConflictsWithInserts(
    const std::vector<SsiPredicate> &predicates, const std::vector<SsiInsert> &inserts) const {
    if (predicates.empty() || inserts.empty()) {
        return false;
    }
    for (const auto &predicate : predicates) {
        for (const auto &insert : inserts) {
            if (insertMatchesPredicate(insert, predicate)) {
                return true;
            }
        }
    }
    return false;
}

bool TransactionManager::isSerializable(TransactionId id) const {
    const auto it = transactions_.find(id);
    if (it == transactions_.end() || it->second.state != TransactionState::Active) {
        return false;
    }
    const auto readIt = activeReads_.find(id);
    const auto writeIt = activeWrites_.find(id);
    const auto predIt = activePredicateReads_.find(id);
    const auto insertIt = activeInserts_.find(id);
    const auto &reads =
        readIt == activeReads_.end() ? std::unordered_set<ConflictKey>{} : readIt->second;
    const auto &writes =
        writeIt == activeWrites_.end() ? std::unordered_set<ConflictKey>{} : writeIt->second;
    const auto &predicates =
        predIt == activePredicateReads_.end() ? std::vector<SsiPredicate>{} : predIt->second;
    const auto &inserts =
        insertIt == activeInserts_.end() ? std::vector<SsiInsert>{} : insertIt->second;

    for (const auto &[seq, committedWrites] : committedWriteSets_) {
        if (seq <= it->second.snapshotMaxCommitSeq) {
            continue;
        }
        if (intersects(reads, committedWrites) || intersects(writes, committedWrites)) {
            return false;
        }
    }
    for (const auto &[seq, committedInserts] : committedInserts_) {
        if (seq <= it->second.snapshotMaxCommitSeq) {
            continue;
        }
        if (predicateConflictsWithInserts(predicates, committedInserts)) {
            return false;
        }
    }
    for (const auto &[seq, committedPredicates] : committedPredicateReads_) {
        if (seq <= it->second.snapshotMaxCommitSeq) {
            continue;
        }
        if (predicateConflictsWithInserts(committedPredicates, inserts)) {
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
    auto predIt = activePredicateReads_.find(id);
    if (predIt != activePredicateReads_.end() && !predIt->second.empty()) {
        committedPredicateReads_.emplace(*it->second.commitSeq, std::move(predIt->second));
    }
    auto insertIt = activeInserts_.find(id);
    if (insertIt != activeInserts_.end() && !insertIt->second.empty()) {
        committedInserts_.emplace(*it->second.commitSeq, std::move(insertIt->second));
    }
    activeWrites_.erase(id);
    activeReads_.erase(id);
    activePredicateReads_.erase(id);
    activeInserts_.erase(id);
    pruneCommittedConflictSets();
}

void TransactionManager::rollback(TransactionId id) {
    auto it = transactions_.find(id);
    if (it == transactions_.end() || it->second.state != TransactionState::Active) {
        throw std::runtime_error("cannot roll back inactive transaction");
    }
    it->second.state = TransactionState::RolledBack;
    activeReads_.erase(id);
    activeWrites_.erase(id);
    activePredicateReads_.erase(id);
    activeInserts_.erase(id);
    pruneCommittedConflictSets();
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

void TransactionManager::recordPredicateRead(TransactionId id, SsiPredicate predicate) {
    if (id == kSystemTransactionId) {
        return;
    }
    auto it = activePredicateReads_.find(id);
    if (it == activePredicateReads_.end()) {
        return;
    }
    it->second.push_back(std::move(predicate));
}

void TransactionManager::recordRelationRead(TransactionId id, std::string_view relation) {
    recordPredicateRead(id, SsiPredicate{std::string{relation}, {}, std::nullopt, std::nullopt,
                                         std::nullopt});
}

void TransactionManager::recordInsert(TransactionId id, SsiInsert insert) {
    if (id == kSystemTransactionId) {
        return;
    }
    auto activeIt = activeInserts_.find(id);
    if (activeIt != activeInserts_.end()) {
        activeIt->second.push_back(std::move(insert));
        return;
    }
    const auto txn = find(id);
    if (!txn || txn->state != TransactionState::Committed || !txn->commitSeq) {
        return;
    }
    committedInserts_[*txn->commitSeq].push_back(std::move(insert));
}

void TransactionManager::pruneCommittedConflictSets() {
    if (activeReads_.empty() && activePredicateReads_.empty() && activeInserts_.empty() &&
        activeWrites_.empty()) {
        committedWriteSets_.clear();
        committedPredicateReads_.clear();
        committedInserts_.clear();
        return;
    }
    CommitSeq minSnapshot = nextCommitSeq_;
    auto consider = [&](TransactionId id) {
        const auto txn = find(id);
        if (txn && txn->state == TransactionState::Active) {
            minSnapshot = std::min(minSnapshot, txn->snapshotMaxCommitSeq);
        }
    };
    for (const auto &[id, _] : activeReads_) {
        consider(id);
    }
    for (const auto &[id, _] : activePredicateReads_) {
        consider(id);
    }
    for (const auto &[id, _] : activeInserts_) {
        consider(id);
    }
    for (const auto &[id, _] : activeWrites_) {
        consider(id);
    }
    auto pruneMap = [&](auto &map) {
        for (auto it = map.begin(); it != map.end();) {
            if (it->first <= minSnapshot) {
                it = map.erase(it);
            } else {
                ++it;
            }
        }
    };
    pruneMap(committedWriteSets_);
    pruneMap(committedPredicateReads_);
    pruneMap(committedInserts_);
}

} // namespace VertexDB
