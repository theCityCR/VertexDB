#pragma once

// Commit sequence and snapshot visibility. Implementation: src/transaction/transaction_manager.cpp.

#include <cstdint>
#include <optional>
#include <unordered_map>

namespace VertexDB {

using TransactionId = std::uint64_t;
using CommitSeq = std::uint64_t;

// Versions stamped with this id are treated as durable system state (loads, direct Table API).
inline constexpr TransactionId kSystemTransactionId = 0;

enum class TransactionState : std::uint8_t {
    Active,
    Committed,
    RolledBack,
};

struct Transaction {
    TransactionId id{};
    TransactionState state{TransactionState::Active};
    std::optional<CommitSeq> commitSeq{};
};

// Snapshot isolation watermark captured at BEGIN (or "now" for autocommit readers).
struct ReadSnapshot {
    TransactionId self{kSystemTransactionId};
    CommitSeq maxCommitSeq{0};
};

class TransactionManager {
  public:
    [[nodiscard]] Transaction begin();
    void commit(TransactionId id);
    void rollback(TransactionId id);
    // Autocommit DML: allocate a transaction id and commit it immediately.
    [[nodiscard]] TransactionId beginCommitted();
    [[nodiscard]] std::optional<Transaction> find(TransactionId id) const;
    [[nodiscard]] CommitSeq currentCommitSeq() const noexcept;
    [[nodiscard]] ReadSnapshot currentSnapshot(TransactionId self = kSystemTransactionId) const;
    [[nodiscard]] bool isCreatorVisible(TransactionId creator, const ReadSnapshot &snapshot) const;
    [[nodiscard]] bool isVisible(TransactionId createdBy, std::optional<TransactionId> deletedBy,
                                 const ReadSnapshot &snapshot) const;

  private:
    TransactionId nextId_{1};
    CommitSeq nextCommitSeq_{0};
    std::unordered_map<TransactionId, Transaction> transactions_;
};

} // namespace VertexDB
