#include "VertexDB/execution/sql_literal.hpp"

#include "VertexDB/common/index_expression.hpp"
#include "VertexDB/execution/foreign_key_eval.hpp"
#include "VertexDB/storage/check_eval.hpp"

#include <sstream>
#include <vector>

namespace VertexDB {

std::string sqlLiteral(const Value &value) {
    if (value.isNull()) {
        return "NULL";
    }
    if (value.isComposite()) {
        return value.toString();
    }
    switch (value.type()) {
    case ColumnType::Int:
        return value.toString();
    case ColumnType::Double: {
        // Ensure the lexeme re-parses as DOUBLE (integers without '.' become INT).
        std::ostringstream out;
        out << std::get<double>(value.data());
        auto text = out.str();
        if (text.find('.') == std::string::npos && text.find('e') == std::string::npos &&
            text.find('E') == std::string::npos) {
            text += ".0";
        }
        return text;
    }
    case ColumnType::String: {
        std::string escaped;
        escaped.reserve(std::get<std::string>(value.data()).size());
        for (const char character : std::get<std::string>(value.data())) {
            if (character == '"' || character == '\\') {
                escaped.push_back('\\');
            }
            escaped.push_back(character);
        }
        return "\"" + escaped + "\"";
    }
    }
    return {};
}

std::string predicateLiteral(const Predicate &predicate) {
    return std::visit(
        [&](const auto &node) -> std::string {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, AndPred> || std::is_same_v<T, OrPred>) {
                const auto op = std::is_same_v<T, AndPred> ? " AND " : " OR ";
                return "(" + predicateLiteral(*node.left) + op +
                       predicateLiteral(*node.right) + ")";
            } else if constexpr (std::is_same_v<T, InListPred>) {
                std::ostringstream out;
                out << node.column << " IN (";
                for (std::size_t i = 0; i < node.inValues.size(); ++i) {
                    if (i > 0) {
                        out << ", ";
                    }
                    out << sqlLiteral(node.inValues[i]);
                }
                out << ")";
                return out.str();
            } else if constexpr (std::is_same_v<T, InSubqueryPred>) {
                return node.column + " IN (SELECT ...)";
            } else if constexpr (std::is_same_v<T, ExistsPred>) {
                return "EXISTS (SELECT ...)";
            } else if constexpr (std::is_same_v<T, LikePred>) {
                return node.column + " LIKE " + sqlLiteral(Value{node.pattern});
            } else if constexpr (std::is_same_v<T, RegexPred>) {
                return node.column + " ~ " + sqlLiteral(Value{node.pattern});
            } else {
                std::string op;
                switch (node.op) {
                case ComparisonOperator::Equal:
                    op = "=";
                    break;
                case ComparisonOperator::Greater:
                    op = ">";
                    break;
                case ComparisonOperator::Less:
                    op = "<";
                    break;
                }
                std::string left = node.column;
                if (node.expression) {
                    left = "(" + indexExpressionToString(*node.expression) + ")";
                }
                if (node.rhsColumn) {
                    return left + " " + op + " " + *node.rhsColumn;
                }
                return left + " " + op + " " + sqlLiteral(node.value);
            }
        },
        predicate);
}

std::string createIndexSql(const CreateIndex &command) {
    if (command.expression) {
        return "CREATE INDEX " + command.name + " ON " + command.table + "((" +
               indexExpressionToString(*command.expression) + "));";
    }
    std::string columns;
    const auto &list = command.columns.empty() ? std::vector<std::string>{command.column}
                                               : command.columns;
    for (std::size_t i = 0; i < list.size(); ++i) {
        if (i != 0) {
            columns += ", ";
        }
        columns += list[i];
    }
    return "CREATE INDEX " + command.name + " ON " + command.table + "(" + columns + ");";
}

std::string dropIndexSql(const DropIndex &command) {
    return "DROP INDEX " + command.name + " ON " + command.table + ";";
}

std::string createTableSql(const CreateTable &command) {
    std::ostringstream sql;
    sql << "CREATE TABLE " << command.name << " (";
    for (std::size_t i = 0; i < command.columns.size(); ++i) {
        if (i != 0) {
            sql << ", ";
        }
        const auto &column = command.columns[i];
        sql << column.name << " " << toString(column.type);
        if (column.nullable) {
            sql << " NULL";
        } else if (!column.primaryKey) {
            // Default columns are NOT NULL; emit the keyword so WAL/replay states the C guarantee.
            sql << " NOT NULL";
        }
        if (column.primaryKey) {
            sql << " PRIMARY KEY";
        } else if (column.unique) {
            sql << " UNIQUE";
        }
    }
    for (const auto &constraint : command.uniqueConstraints) {
        sql << ", " << (constraint.primaryKey ? "PRIMARY KEY (" : "UNIQUE (");
        for (std::size_t i = 0; i < constraint.columns.size(); ++i) {
            if (i != 0) {
                sql << ", ";
            }
            sql << constraint.columns[i];
        }
        sql << ")";
    }
    for (const auto &check : command.checkConstraints) {
        sql << ", CHECK (" << checkConstraintLiteral(check) << ")";
    }
    for (const auto &fk : command.foreignKeys) {
        sql << ", " << foreignKeyLiteral(fk);
    }
    sql << ");";
    return sql.str();
}

std::string insertSql(std::string_view table, const Row &row) {
    std::ostringstream sql;
    sql << "INSERT INTO " << table << " VALUES (";
    for (std::size_t i = 0; i < row.size(); ++i) {
        if (i != 0) {
            sql << ", ";
        }
        sql << sqlLiteral(row[i]);
    }
    sql << ");";
    return sql.str();
}

std::string updateSql(const Update &command) {
    std::ostringstream sql;
    sql << "UPDATE " << command.table << " SET " << command.column << " = "
        << sqlLiteral(command.value);
    if (command.where) {
        sql << " WHERE " << predicateLiteral(*command.where);
    }
    sql << ";";
    return sql.str();
}

std::string deleteSql(const Delete &command) {
    std::ostringstream sql;
    sql << "DELETE FROM " << command.table;
    if (command.where) {
        sql << " WHERE " << predicateLiteral(*command.where);
    }
    sql << ";";
    return sql.str();
}

} // namespace VertexDB
