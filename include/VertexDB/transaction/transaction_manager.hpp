#pragma once

// Commit sequence, snapshot visibility, and row-level SSI conflict checks.
// Implementation: src/transaction/transaction_manager.cpp.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

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

// Thrown when commit would create a dangerous rw/ww structure under row-level SSI.
class SerializationFailure : public std::runtime_error {
  public:
    explicit SerializationFailure(
        const std::string &message = "serialization failure: concurrent read/write conflict")
        : std::runtime_error(message) {}
};

struct Transaction {
    TransactionId id{};
    TransactionState state{TransactionState::Active};
    std::optional<CommitSeq> commitSeq{};
    // Snapshot watermark captured at BEGIN (SI); used by SSI commit checks.
    CommitSeq snapshotMaxCommitSeq{0};
};

// Snapshot isolation watermark captured at BEGIN (or "now" for autocommit readers).
struct ReadSnapshot {
    TransactionId self{kSystemTransactionId};
    CommitSeq maxCommitSeq{0};
};

// Opaque (relation, row) key for SSI read/write sets.
using ConflictKey = std::string;

class TransactionManager {
  public:
    [[nodiscard]] Transaction begin();
    // Commits when row-level SSI allows; otherwise marks the txn rolled back and throws
    // SerializationFailure (first-committer wins for overlapping read/write sets).
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

    // Row-level SSI tracking (no predicate locks): relation+row read/write sets.
    void recordRead(TransactionId id, std::string_view relation, std::size_t rowId);
    void recordWrite(TransactionId id, std::string_view relation, std::size_t rowId);
    [[nodiscard]] bool isSerializable(TransactionId id) const;
    [[nodiscard]] static ConflictKey conflictKey(std::string_view relation, std::size_t rowId);

  private:
    void pruneCommittedWriteSets();

    TransactionId nextId_{1};
    CommitSeq nextCommitSeq_{0};
    std::unordered_map<TransactionId, Transaction> transactions_;
    std::unordered_map<TransactionId, std::unordered_set<ConflictKey>> activeReads_;
    std::unordered_map<TransactionId, std::unordered_set<ConflictKey>> activeWrites_;
    // Write sets of committed txns retained until no active snapshot can conflict with them.
    std::unordered_map<CommitSeq, std::unordered_set<ConflictKey>> committedWriteSets_;
};

} // namespace VertexDB
