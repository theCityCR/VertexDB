#pragma once

// Typed SQL AST (Query variant and statement structs). Built by Parser; consumed
// by QueryExecutor / QueryPlanner / rewriter.

#include "VertexDB/common/comparison_operator.hpp"
#include "VertexDB/common/index_expression.hpp"
#include "VertexDB/common/value.hpp"
#include "VertexDB/parser/predicate.hpp"

#include <cstdint>
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

struct CteEntry {
    std::string name;
    std::shared_ptr<Select> body;
    MaterializeMode materializeMode{MaterializeMode::DefaultInline};
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

enum class AggregateOp : std::uint8_t {
    CountStar,
    Count,
    Sum,
    Avg,
    Min,
    Max,
};

struct SelectExpr {
    enum class Kind : std::uint8_t { Star, Column, Aggregate };

    Kind kind{Kind::Column};
    std::string column;
    AggregateOp aggregate{AggregateOp::CountStar};
    std::optional<std::string> aggregateArg;

    [[nodiscard]] static SelectExpr makeStar() {
        SelectExpr expr;
        expr.kind = Kind::Star;
        return expr;
    }
    [[nodiscard]] static SelectExpr makeColumn(std::string name) {
        SelectExpr expr;
        expr.kind = Kind::Column;
        expr.column = std::move(name);
        return expr;
    }
    [[nodiscard]] static SelectExpr makeAggregate(AggregateOp op,
                                                  std::optional<std::string> arg = std::nullopt) {
        SelectExpr expr;
        expr.kind = Kind::Aggregate;
        expr.aggregate = op;
        expr.aggregateArg = std::move(arg);
        return expr;
    }

    [[nodiscard]] std::string outputName() const {
        switch (kind) {
        case Kind::Star:
            return "*";
        case Kind::Column:
            return column;
        case Kind::Aggregate:
            switch (aggregate) {
            case AggregateOp::CountStar:
                return "COUNT(*)";
            case AggregateOp::Count:
                return "COUNT(" + aggregateArg.value_or("") + ")";
            case AggregateOp::Sum:
                return "SUM(" + aggregateArg.value_or("") + ")";
            case AggregateOp::Avg:
                return "AVG(" + aggregateArg.value_or("") + ")";
            case AggregateOp::Min:
                return "MIN(" + aggregateArg.value_or("") + ")";
            case AggregateOp::Max:
                return "MAX(" + aggregateArg.value_or("") + ")";
            }
        }
        return {};
    }

    [[nodiscard]] friend bool operator==(const SelectExpr &lhs, const SelectExpr &rhs) = default;
};

[[nodiscard]] inline bool isStarProjection(const std::vector<SelectExpr> &columns) {
    return columns.size() == 1 && columns.front().kind == SelectExpr::Kind::Star;
}

[[nodiscard]] inline bool hasAggregates(const std::vector<SelectExpr> &columns) {
    for (const auto &column : columns) {
        if (column.kind == SelectExpr::Kind::Aggregate) {
            return true;
        }
    }
    return false;
}

struct Select {
    std::string table;
    std::vector<JoinClause> joins;
    std::vector<SelectExpr> columns;
    std::optional<Predicate> where;
    std::vector<std::string> groupBy;
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
struct Analyze {
    // Empty means analyze every table in the active database.
    std::optional<std::string> table;
};
struct Exit {};

using Query =
    std::variant<CreateDatabase, CreateTable, DropTable, RenameTable, ListTables, Insert, Select,
                 Update, Delete, CreateIndex, SaveDatabase, LoadDatabase, BeginTransaction,
                 CommitTransaction, RollbackTransaction, PrepareStatement, ExecutePrepared,
                 ExplainQuery, Analyze, Exit>;

} // namespace VertexDB
