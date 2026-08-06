#include "VertexDB/execution/sql_literal.hpp"

#include "VertexDB/common/index_expression.hpp"

#include <sstream>

namespace VertexDB {

std::string sqlLiteral(const Value &value) {
    if (value.isNull()) {
        return "NULL";
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
    if (predicate.kind == Predicate::Kind::And || predicate.kind == Predicate::Kind::Or) {
        const auto op = predicate.kind == Predicate::Kind::And ? " AND " : " OR ";
        return "(" + predicateLiteral(*predicate.left) + op + predicateLiteral(*predicate.right) +
               ")";
    }
    if (predicate.kind == Predicate::Kind::InList) {
        std::ostringstream out;
        out << predicate.column << " IN (";
        for (std::size_t i = 0; i < predicate.inValues.size(); ++i) {
            if (i > 0) {
                out << ", ";
            }
            out << sqlLiteral(predicate.inValues[i]);
        }
        out << ")";
        return out.str();
    }
    if (predicate.kind == Predicate::Kind::InSubquery) {
        return predicate.column + " IN (SELECT ...)";
    }
    if (predicate.kind == Predicate::Kind::Exists) {
        return "EXISTS (SELECT ...)";
    }

    std::string op;
    switch (predicate.op) {
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
    std::string left = predicate.column;
    if (predicate.expression) {
        left = "(" + indexExpressionToString(*predicate.expression) + ")";
    }
    if (predicate.rhsColumn) {
        return left + " " + op + " " + *predicate.rhsColumn;
    }
    return left + " " + op + " " + sqlLiteral(predicate.value);
}

std::string createIndexSql(const CreateIndex &command) {
    if (command.expression) {
        return "CREATE INDEX " + command.name + " ON " + command.table + "((" +
               indexExpressionToString(*command.expression) + "));";
    }
    return "CREATE INDEX " + command.name + " ON " + command.table + "(" + command.column + ");";
}

std::string createTableSql(const CreateTable &command) {
    std::ostringstream sql;
    sql << "CREATE TABLE " << command.name << " (";
    for (std::size_t i = 0; i < command.columns.size(); ++i) {
        if (i != 0) {
            sql << ", ";
        }
        sql << command.columns[i].name << " " << toString(command.columns[i].type);
        if (command.columns[i].nullable) {
            sql << " NULL";
        }
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
