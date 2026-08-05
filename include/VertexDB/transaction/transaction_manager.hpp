#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

namespace VertexDB {

using TransactionId = std::uint64_t;

enum class TransactionState : std::uint8_t {
    Active,
    Committed,
    RolledBack,
};

struct Transaction {
    TransactionId id{};
    TransactionState state{TransactionState::Active};
};

class TransactionManager {
  public:
    [[nodiscard]] Transaction begin();
    void commit(TransactionId id);
    void rollback(TransactionId id);
    [[nodiscard]] std::optional<Transaction> find(TransactionId id) const;

  private:
    TransactionId nextId_{1};
    std::unordered_map<TransactionId, Transaction> transactions_;
};

} // namespace VertexDB
