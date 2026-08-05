#pragma once

#include "VertexDB/common/comparison_operator.hpp"
#include "VertexDB/common/value.hpp"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace VertexDB {

struct Predicate {
    enum class Kind {
        Comparison,
        And,
        Or,
    };

    Predicate() = default;
    Predicate(std::string columnName, ComparisonOperator comparison, Value comparisonValue)
        : column(std::move(columnName)), op(comparison), value(std::move(comparisonValue)) {}
    Predicate(Kind predicateKind, std::shared_ptr<Predicate> leftPredicate,
              std::shared_ptr<Predicate> rightPredicate)
        : kind(predicateKind), left(std::move(leftPredicate)), right(std::move(rightPredicate)) {}

    Kind kind{Kind::Comparison};
    std::string column;
    ComparisonOperator op;
    Value value;
    std::shared_ptr<Predicate> left;
    std::shared_ptr<Predicate> right;
};

struct CreateDatabase {
    std::string name;
};

struct CreateTable {
    std::string name;
    std::vector<Column> columns;
};

struct DropTable {
    std::string name;
};

struct RenameTable {
    std::string oldName;
    std::string newName;
};

struct ListTables {};

struct Insert {
    std::string table;
    std::vector<std::vector<Value>> rows;
};

struct OrderBy {
    std::string column;
    bool ascending{true};
};

struct JoinClause {
    std::string table;
    std::string leftColumn;
    std::string rightColumn;
};

struct Select {
    std::string table;
    std::optional<JoinClause> join;
    std::vector<std::string> columns;
    std::optional<Predicate> where;
    std::optional<OrderBy> orderBy;
    std::optional<std::size_t> limit;
};

struct Update {
    std::string table;
    std::string column;
    Value value;
    std::optional<Predicate> where;
};

struct Delete {
    std::string table;
    std::optional<Predicate> where;
};

struct CreateIndex {
    std::string name;
    std::string table;
    std::string column;
};

struct SaveDatabase {};
struct LoadDatabase {
    std::optional<std::string> name;
};
struct BeginTransaction {};
struct CommitTransaction {};
struct RollbackTransaction {};
struct PrepareStatement {
    std::string name;
    std::string sql;
};
struct ExecutePrepared {
    std::string name;
    std::vector<Value> parameters;
};
struct Exit {};

using Query =
    std::variant<CreateDatabase, CreateTable, DropTable, RenameTable, ListTables, Insert, Select,
                 Update, Delete, CreateIndex, SaveDatabase, LoadDatabase, BeginTransaction,
                 CommitTransaction, RollbackTransaction, PrepareStatement, ExecutePrepared, Exit>;

} // namespace VertexDB
