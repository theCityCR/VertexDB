#include "VertexDB/execution/query_executor.hpp"

#include "VertexDB/execution/prepared_bind.hpp"
#include "VertexDB/execution/select_helpers.hpp"
#include "VertexDB/parser/parser.hpp"

#include <stdexcept>
#include <utility>

namespace VertexDB {

QueryExecutor::QueryExecutor(std::filesystem::path storageRoot)
    : storageManager_(storageRoot), wal_(storageRoot / "VertexDB.wal"),
      recovery_(storageManager_, wal_, session_, database_,
                [this](const Query &query) { (void)executeUnlocked(query); }),
      ctx_{database_, planner_, session_}, selectEngine_(ctx_), subqueryRuntime_(ctx_),
      dmlEngine_(ctx_, recovery_),
      catalogEngine_(ctx_, recovery_, storageManager_, wal_) {
    ctx_.select = &selectEngine_;
    ctx_.subquery = &subqueryRuntime_;
    recovery_.recoverFromStorage();
}

QueryResult QueryExecutor::execute(const Query &query) {
    if (std::holds_alternative<ExecutePrepared>(query)) {
        return executePrepared(std::get<ExecutePrepared>(query));
    }

    const bool readOnly = std::holds_alternative<Select>(query) ||
                          std::holds_alternative<ListTables>(query) ||
                          std::holds_alternative<ExplainQuery>(query);
    if (readOnly) {
        const auto lock = lockManager_.acquireRead();
        return executeUnlocked(query);
    }

    const auto lock = lockManager_.acquireWrite();
    return executeUnlocked(query);
}

QueryResult QueryExecutor::executeUnlocked(const Query &query) {
    return std::visit(
        [this](const auto &command) -> QueryResult {
            using Command = std::decay_t<decltype(command)>;
            if constexpr (std::is_same_v<Command, CreateDatabase>) {
                return catalogEngine_.executeCreateDatabase(command);
            } else if constexpr (std::is_same_v<Command, DropDatabase>) {
                return catalogEngine_.executeDropDatabase(command);
            } else if constexpr (std::is_same_v<Command, CreateTable>) {
                return catalogEngine_.executeCreateTable(command);
            } else if constexpr (std::is_same_v<Command, DropTable>) {
                return catalogEngine_.executeDropTable(command);
            } else if constexpr (std::is_same_v<Command, RenameTable>) {
                return catalogEngine_.executeRenameTable(command);
            } else if constexpr (std::is_same_v<Command, ListTables>) {
                return catalogEngine_.executeListTables();
            } else if constexpr (std::is_same_v<Command, Insert>) {
                return executeInsert(command);
            } else if constexpr (std::is_same_v<Command, Select>) {
                return executeSelect(command);
            } else if constexpr (std::is_same_v<Command, ExplainQuery>) {
                return executeExplain(command);
            } else if constexpr (std::is_same_v<Command, Update>) {
                return executeUpdate(command);
            } else if constexpr (std::is_same_v<Command, Delete>) {
                return executeDelete(command);
            } else if constexpr (std::is_same_v<Command, CreateIndex>) {
                return catalogEngine_.executeCreateIndex(command);
            } else if constexpr (std::is_same_v<Command, DropIndex>) {
                return catalogEngine_.executeDropIndex(command);
            } else if constexpr (std::is_same_v<Command, Analyze>) {
                return catalogEngine_.executeAnalyze(command);
            } else if constexpr (std::is_same_v<Command, SaveDatabase>) {
                return catalogEngine_.executeSaveDatabase();
            } else if constexpr (std::is_same_v<Command, LoadDatabase>) {
                return catalogEngine_.executeLoadDatabase(command);
            } else if constexpr (std::is_same_v<Command, BeginTransaction>) {
                return executeBegin();
            } else if constexpr (std::is_same_v<Command, CommitTransaction>) {
                return executeCommit();
            } else if constexpr (std::is_same_v<Command, RollbackTransaction>) {
                return executeRollback();
            } else if constexpr (std::is_same_v<Command, PrepareStatement>) {
                return executePrepare(command);
            } else if constexpr (std::is_same_v<Command, ExecutePrepared>) {
                throw std::runtime_error("prepared execution must not be dispatched while locked");
            } else if constexpr (std::is_same_v<Command, Exit>) {
                return messageResult(true, "exit");
            }
        },
        query);
}

std::shared_ptr<Database> QueryExecutor::currentDatabase() const noexcept { return database_; }

QueryResult QueryExecutor::executeSelect(const Select &command) {
    return selectEngine_.execute(command);
}

QueryResult QueryExecutor::executeExplain(const ExplainQuery &command) {
    return selectEngine_.explain(command);
}

QueryResult QueryExecutor::executeInsert(const Insert &command) {
    return dmlEngine_.executeInsert(command);
}

QueryResult QueryExecutor::executeUpdate(const Update &command) {
    return dmlEngine_.executeUpdate(command);
}

QueryResult QueryExecutor::executeDelete(const Delete &command) {
    return dmlEngine_.executeDelete(command);
}

QueryResult QueryExecutor::executeBegin() {
    if (!database_) {
        return messageResult(false, "no active database");
    }
    return session_.begin();
}

void QueryExecutor::armCrashInjection(CrashInjectionPoint point) noexcept {
    crashInjection_ = point;
}

void QueryExecutor::fireCrashInjection(CrashInjectionPoint point) {
    crashInjection_ = CrashInjectionPoint::None;
    throw CrashInjected{point};
}

QueryResult QueryExecutor::executeCommit() {
    if (!session_.transactionActive()) {
        return messageResult(false, "no active transaction");
    }
    if (!session_.transactionManager().isSerializable(*session_.activeTransactionId())) {
        while (auto record = session_.undoLog().pop()) {
            recovery_.applyUndoRecord(*record);
        }
        session_.clearPendingWal();
        (void)session_.rollback();
        return messageResult(false, "serialization failure");
    }
    if (crashInjection_ == CrashInjectionPoint::BeforeWalSync) {
        fireCrashInjection(CrashInjectionPoint::BeforeWalSync);
    }
    recovery_.flushPendingWal();
    if (crashInjection_ == CrashInjectionPoint::AfterWalSyncBeforeCommitMark) {
        fireCrashInjection(CrashInjectionPoint::AfterWalSyncBeforeCommitMark);
    }
    return session_.commit();
}

QueryResult QueryExecutor::executeRollback() {
    if (!session_.transactionActive()) {
        return messageResult(false, "no active transaction");
    }
    while (auto record = session_.undoLog().pop()) {
        recovery_.applyUndoRecord(*record);
    }
    return session_.rollback();
}

QueryResult QueryExecutor::executePrepare(const PrepareStatement &command) {
    // Parse once into a typed AST with Parameter slots; EXECUTE binds without reparsing.
    auto ast = Parser{}.parse(command.sql);
    prepared_.store(command.name, std::move(ast));
    return messageResult(true, "prepared statement " + command.name);
}

QueryResult QueryExecutor::executePrepared(const ExecutePrepared &command) {
    Query stored;
    {
        const auto lock = lockManager_.acquireRead();
        auto prepared = prepared_.find(command.name);
        if (!prepared) {
            throw std::runtime_error("unknown prepared statement");
        }
        stored = std::move(*prepared);
    }
    const Query bound = bindQueryParameters(stored, command.parameters);
    return execute(bound);
}

std::optional<Query> QueryExecutor::preparedAst(std::string_view name) const {
    const auto lock = lockManager_.acquireRead();
    return prepared_.findCaseInsensitive(name);
}

} // namespace VertexDB
