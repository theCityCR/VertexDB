#include "VertexDB/transaction/transaction_manager.hpp"

#include <stdexcept>

namespace VertexDB {

Transaction TransactionManager::begin() {
    Transaction transaction{nextId_++, TransactionState::Active};
    transactions_.emplace(transaction.id, transaction);
    return transaction;
}

void TransactionManager::commit(TransactionId id) {
    auto it = transactions_.find(id);
    if (it == transactions_.end() || it->second.state != TransactionState::Active) {
        throw std::runtime_error("cannot commit inactive transaction");
    }
    it->second.state = TransactionState::Committed;
}

void TransactionManager::rollback(TransactionId id) {
    auto it = transactions_.find(id);
    if (it == transactions_.end() || it->second.state != TransactionState::Active) {
        throw std::runtime_error("cannot roll back inactive transaction");
    }
    it->second.state = TransactionState::RolledBack;
}

std::optional<Transaction> TransactionManager::find(TransactionId id) const {
    auto it = transactions_.find(id);
    if (it == transactions_.end()) {
        return std::nullopt;
    }
    return it->second;
}

} // namespace VertexDB
