#pragma once

#include "VertexDB/common/comparison_operator.hpp"
#include "VertexDB/common/value.hpp"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace VertexDB {

struct Select;

enum class MaterializeMode {
    DefaultInline,
    Materialized,
    NotMaterialized,
};

struct IndexExpression {
    enum class Kind { Column, Negate, Add, Subtract };

    Kind kind{Kind::Column};
    std::string column;
    Value literal{};

    [[nodiscard]] friend bool operator==(const IndexExpression &lhs,
                                         const IndexExpression &rhs) = default;
};

struct CteEntry {
    std::string name;
    std::shared_ptr<Select> body;
    MaterializeMode materializeMode{MaterializeMode::DefaultInline};
};

struct Predicate {
    enum class Kind {
        Comparison,
        And,
        Or,
        InSubquery,
        InList,
        Exists,
    };

    Predicate() = default;
    Predicate(std::string columnName, ComparisonOperator comparison, Value comparisonValue)
        : column(std::move(columnName)), op(comparison), value(std::move(comparisonValue)) {}
    Predicate(Kind predicateKind, std::shared_ptr<Predicate> leftPredicate,
              std::shared_ptr<Predicate> rightPredicate)
        : kind(predicateKind), left(std::move(leftPredicate)), right(std::move(rightPredicate)) {}
    Predicate(std::string columnName, std::shared_ptr<Select> sub)
        : kind(Kind::InSubquery), column(std::move(columnName)), subquery(std::move(sub)) {}
    Predicate(std::string columnName, std::vector<Value> values)
        : kind(Kind::InList), column(std::move(columnName)), inValues(std::move(values)) {}
    static Predicate makeExists(std::shared_ptr<Select> sub) {
        Predicate predicate;
        predicate.kind = Kind::Exists;
        predicate.subquery = std::move(sub);
        return predicate;
    }
    static Predicate makeExpressionComparison(IndexExpression expr, ComparisonOperator comparison,
                                              Value comparisonValue) {
        Predicate predicate;
        predicate.kind = Kind::Comparison;
        predicate.expression = std::move(expr);
        predicate.column = predicate.expression->column;
        predicate.op = comparison;
        predicate.value = std::move(comparisonValue);
        return predicate;
    }

    Kind kind{Kind::Comparison};
    std::string column;
    ComparisonOperator op{};
    Value value;
    std::shared_ptr<Predicate> left;
    std::shared_ptr<Predicate> right;
    std::shared_ptr<Select> subquery;
    std::vector<Value> inValues;
    // When set, Comparison compares column/expression against an outer (or unqualified) column
    // instead of a literal value.
    std::optional<std::string> rhsColumn;
    std::optional<IndexExpression> expression;
    bool referencesOuter{false};
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
    // CTE bodies are stored by shared_ptr to avoid an incomplete-type cycle.
    std::vector<CteEntry> ctes;
    bool hasOuterRefs{false};
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
    std::optional<IndexExpression> expression;
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
struct ExplainQuery {
    Select query;
};
struct Exit {};

using Query =
    std::variant<CreateDatabase, CreateTable, DropTable, RenameTable, ListTables, Insert, Select,
                 Update, Delete, CreateIndex, SaveDatabase, LoadDatabase, BeginTransaction,
                 CommitTransaction, RollbackTransaction, PrepareStatement, ExecutePrepared,
                 ExplainQuery, Exit>;

} // namespace VertexDB
