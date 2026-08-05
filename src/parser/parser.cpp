#include "VertexDB/parser/parser.hpp"

#include "VertexDB/parser/tokenizer.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <stdexcept>

namespace VertexDB {
namespace {

bool equalsIgnoreCase(std::string_view lhs, std::string_view rhs) {
    return lhs.size() == rhs.size() && std::ranges::equal(lhs, rhs, [](char left, char right) {
               return std::toupper(static_cast<unsigned char>(left)) ==
                      std::toupper(static_cast<unsigned char>(right));
           });
}

std::int64_t parseInt(std::string_view text) {
    std::int64_t result{};
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), result);
    if (ec != std::errc{} || ptr != text.data() + text.size()) {
        throw std::runtime_error("invalid integer literal");
    }
    return result;
}

} // namespace

Query Parser::parse(std::string_view sql) {
    const auto tokens = Tokenizer{}.tokenize(sql);
    return parse(tokens);
}

Query Parser::parse(std::span<const Token> tokens) {
    tokens_ = tokens;
    current_ = 0;

    auto finish = [this](Query query) {
        expectStatementEnd();
        return query;
    };

    if (match(TokenType::Identifier, "CREATE")) {
        if (match(TokenType::Identifier, "DATABASE")) {
            return finish(parseCreateDatabase());
        }
        if (match(TokenType::Identifier, "TABLE")) {
            return finish(parseCreateTable());
        }
        if (match(TokenType::Identifier, "INDEX")) {
            return finish(parseCreateIndex());
        }
        throw std::runtime_error("expected DATABASE, TABLE, or INDEX after CREATE");
    }
    if (match(TokenType::Identifier, "DROP")) {
        return finish(parseDropTable());
    }
    if (match(TokenType::Identifier, "RENAME")) {
        return finish(parseRenameTable());
    }
    if (match(TokenType::Identifier, "LIST")) {
        expect(TokenType::Identifier, "TABLES");
        return finish(ListTables{});
    }
    if (match(TokenType::Identifier, "INSERT")) {
        return finish(parseInsert());
    }
    if (match(TokenType::Identifier, "SELECT")) {
        return finish(parseSelect());
    }
    if (match(TokenType::Identifier, "UPDATE")) {
        return finish(parseUpdate());
    }
    if (match(TokenType::Identifier, "DELETE")) {
        return finish(parseDelete());
    }
    if (match(TokenType::Identifier, "SAVE")) {
        expect(TokenType::Identifier, "DATABASE");
        return finish(SaveDatabase{});
    }
    if (match(TokenType::Identifier, "LOAD")) {
        expect(TokenType::Identifier, "DATABASE");
        if (peek().type == TokenType::Identifier) {
            return finish(LoadDatabase{advance().lexeme});
        }
        return finish(LoadDatabase{});
    }
    if (match(TokenType::Identifier, "BEGIN")) {
        return finish(BeginTransaction{});
    }
    if (match(TokenType::Identifier, "COMMIT")) {
        return finish(CommitTransaction{});
    }
    if (match(TokenType::Identifier, "ROLLBACK")) {
        return finish(RollbackTransaction{});
    }
    if (match(TokenType::Identifier, "PREPARE")) {
        return finish(parsePrepare());
    }
    if (match(TokenType::Identifier, "EXECUTE")) {
        return finish(parseExecutePrepared());
    }
    if (match(TokenType::Identifier, "EXIT")) {
        return finish(Exit{});
    }

    throw std::runtime_error("unsupported SQL command");
}

const Token &Parser::peek() const {
    if (current_ >= tokens_.size()) {
        throw std::runtime_error("parser read past end of token stream");
    }
    return tokens_[current_];
}

const Token &Parser::advance() {
    const Token &token = peek();
    if (token.type != TokenType::End) {
        ++current_;
    }
    return token;
}

bool Parser::match(TokenType type, std::string_view lexeme) {
    const Token &token = peek();
    if (token.type != type) {
        return false;
    }
    if (!lexeme.empty() && !equalsIgnoreCase(token.lexeme, lexeme)) {
        return false;
    }
    (void)advance();
    return true;
}

void Parser::expect(TokenType type, std::string_view lexeme) {
    if (!match(type, lexeme)) {
        throw std::runtime_error("unexpected token");
    }
}

void Parser::expectStatementEnd() {
    (void)match(TokenType::Semicolon);
    if (peek().type != TokenType::End) {
        throw std::runtime_error("unexpected trailing token");
    }
}

CreateDatabase Parser::parseCreateDatabase() {
    const auto name = advance();
    if (name.type != TokenType::Identifier) {
        throw std::runtime_error("expected database name");
    }
    return {name.lexeme};
}

CreateTable Parser::parseCreateTable() {
    const auto table = advance();
    if (table.type != TokenType::Identifier) {
        throw std::runtime_error("expected table name");
    }
    expect(TokenType::LeftParen);

    std::vector<Column> columns;
    do {
        const auto columnName = advance();
        if (columnName.type != TokenType::Identifier) {
            throw std::runtime_error("expected column name");
        }
        const auto typeName = advance();
        if (typeName.type != TokenType::Identifier) {
            throw std::runtime_error("expected column type");
        }
        auto type = columnTypeFromString(typeName.lexeme);
        if (!type) {
            throw std::runtime_error("unsupported column type");
        }
        bool nullable = false;
        if (match(TokenType::Identifier, "NOT")) {
            expect(TokenType::Identifier, "NULL");
        } else if (match(TokenType::Identifier, "NULL")) {
            nullable = true;
        }
        columns.push_back({columnName.lexeme, *type, nullable});
    } while (match(TokenType::Comma));

    expect(TokenType::RightParen);
    return {table.lexeme, std::move(columns)};
}

DropTable Parser::parseDropTable() {
    expect(TokenType::Identifier, "TABLE");
    const auto table = advance();
    if (table.type != TokenType::Identifier) {
        throw std::runtime_error("expected table name");
    }
    return {table.lexeme};
}

RenameTable Parser::parseRenameTable() {
    expect(TokenType::Identifier, "TABLE");
    const auto oldName = advance();
    if (oldName.type != TokenType::Identifier) {
        throw std::runtime_error("expected source table name");
    }
    expect(TokenType::Identifier, "TO");
    const auto newName = advance();
    if (newName.type != TokenType::Identifier) {
        throw std::runtime_error("expected destination table name");
    }
    return {oldName.lexeme, newName.lexeme};
}

CreateIndex Parser::parseCreateIndex() {
    const auto index = advance();
    if (index.type != TokenType::Identifier) {
        throw std::runtime_error("expected index name");
    }
    expect(TokenType::Identifier, "ON");
    const auto table = advance();
    if (table.type != TokenType::Identifier) {
        throw std::runtime_error("expected indexed table name");
    }
    expect(TokenType::LeftParen);
    const auto column = advance();
    if (column.type != TokenType::Identifier) {
        throw std::runtime_error("expected indexed column name");
    }
    expect(TokenType::RightParen);
    return {index.lexeme, table.lexeme, column.lexeme};
}

Insert Parser::parseInsert() {
    expect(TokenType::Identifier, "INTO");
    const auto table = advance();
    if (table.type != TokenType::Identifier) {
        throw std::runtime_error("expected table name");
    }
    expect(TokenType::Identifier, "VALUES");
    std::vector<std::vector<Value>> rows;
    do {
        expect(TokenType::LeftParen);
        std::vector<Value> values;
        do {
            values.push_back(parseValue());
        } while (match(TokenType::Comma));
        expect(TokenType::RightParen);
        rows.push_back(std::move(values));
    } while (match(TokenType::Comma));
    return {table.lexeme, std::move(rows)};
}

Select Parser::parseSelect() {
    std::vector<std::string> columns;
    if (match(TokenType::Star)) {
        columns.emplace_back("*");
    } else {
        do {
            const auto column = advance();
            if (column.type != TokenType::Identifier) {
                throw std::runtime_error("expected selected column");
            }
            columns.push_back(column.lexeme);
        } while (match(TokenType::Comma));
    }

    expect(TokenType::Identifier, "FROM");
    const auto table = advance();
    if (table.type != TokenType::Identifier) {
        throw std::runtime_error("expected table name");
    }

    std::optional<JoinClause> join;
    if (match(TokenType::Identifier, "JOIN")) {
        const auto joinedTable = advance();
        if (joinedTable.type != TokenType::Identifier) {
            throw std::runtime_error("expected joined table name");
        }
        expect(TokenType::Identifier, "ON");
        const auto leftColumn = advance();
        if (leftColumn.type != TokenType::Identifier) {
            throw std::runtime_error("expected left join column");
        }
        expect(TokenType::Equal);
        const auto rightColumn = advance();
        if (rightColumn.type != TokenType::Identifier) {
            throw std::runtime_error("expected right join column");
        }
        join = JoinClause{joinedTable.lexeme, leftColumn.lexeme, rightColumn.lexeme};
    }

    std::optional<Predicate> where;
    std::optional<OrderBy> orderBy;
    std::optional<std::size_t> limit;
    if (match(TokenType::Identifier, "WHERE")) {
        where = parsePredicate();
    }
    if (match(TokenType::Identifier, "ORDER")) {
        expect(TokenType::Identifier, "BY");
        const auto column = advance();
        if (column.type != TokenType::Identifier) {
            throw std::runtime_error("expected ORDER BY column");
        }
        bool ascending = true;
        if (match(TokenType::Identifier, "DESC")) {
            ascending = false;
        } else {
            (void)match(TokenType::Identifier, "ASC");
        }
        orderBy = OrderBy{column.lexeme, ascending};
    }
    if (match(TokenType::Identifier, "LIMIT")) {
        const auto count = advance();
        if (count.type != TokenType::Number) {
            throw std::runtime_error("expected numeric limit");
        }
        limit = static_cast<std::size_t>(parseInt(count.lexeme));
    }
    return {table.lexeme,     std::move(join),    std::move(columns),
            std::move(where), std::move(orderBy), limit};
}

Update Parser::parseUpdate() {
    const auto table = advance();
    if (table.type != TokenType::Identifier) {
        throw std::runtime_error("expected table name");
    }
    expect(TokenType::Identifier, "SET");
    const auto column = advance();
    if (column.type != TokenType::Identifier) {
        throw std::runtime_error("expected update column");
    }
    expect(TokenType::Equal);
    auto value = parseValue();
    std::optional<Predicate> where;
    if (match(TokenType::Identifier, "WHERE")) {
        where = parsePredicate();
    }
    return {table.lexeme, column.lexeme, std::move(value), std::move(where)};
}

Delete Parser::parseDelete() {
    expect(TokenType::Identifier, "FROM");
    const auto table = advance();
    if (table.type != TokenType::Identifier) {
        throw std::runtime_error("expected table name");
    }
    std::optional<Predicate> where;
    if (match(TokenType::Identifier, "WHERE")) {
        where = parsePredicate();
    }
    return {table.lexeme, std::move(where)};
}

PrepareStatement Parser::parsePrepare() {
    const auto name = advance();
    if (name.type != TokenType::Identifier) {
        throw std::runtime_error("expected prepared statement name");
    }
    expect(TokenType::Identifier, "AS");
    const auto sql = advance();
    if (sql.type != TokenType::String) {
        throw std::runtime_error("expected prepared SQL string");
    }
    return {name.lexeme, sql.lexeme};
}

ExecutePrepared Parser::parseExecutePrepared() {
    const auto name = advance();
    if (name.type != TokenType::Identifier) {
        throw std::runtime_error("expected prepared statement name");
    }

    std::vector<Value> parameters;
    if (match(TokenType::Identifier, "VALUES")) {
        expect(TokenType::LeftParen);
        do {
            parameters.push_back(parseValue());
        } while (match(TokenType::Comma));
        expect(TokenType::RightParen);
    }
    return {name.lexeme, std::move(parameters)};
}

Predicate Parser::parsePredicate() { return parseOrPredicate(); }

Predicate Parser::parseOrPredicate() {
    auto predicate = parseAndPredicate();
    while (match(TokenType::Identifier, "OR")) {
        auto left = std::make_shared<Predicate>(std::move(predicate));
        auto right = std::make_shared<Predicate>(parseAndPredicate());
        predicate = Predicate{Predicate::Kind::Or, std::move(left), std::move(right)};
    }
    return predicate;
}

Predicate Parser::parseAndPredicate() {
    auto predicate = parsePrimaryPredicate();
    while (match(TokenType::Identifier, "AND")) {
        auto left = std::make_shared<Predicate>(std::move(predicate));
        auto right = std::make_shared<Predicate>(parsePrimaryPredicate());
        predicate = Predicate{Predicate::Kind::And, std::move(left), std::move(right)};
    }
    return predicate;
}

Predicate Parser::parsePrimaryPredicate() {
    if (match(TokenType::LeftParen)) {
        auto predicate = parsePredicate();
        expect(TokenType::RightParen);
        return predicate;
    }
    return parseComparisonPredicate();
}

Predicate Parser::parseComparisonPredicate() {
    const auto column = advance();
    if (column.type != TokenType::Identifier) {
        throw std::runtime_error("expected predicate column");
    }
    ComparisonOperator op{};
    if (match(TokenType::Equal)) {
        op = ComparisonOperator::Equal;
    } else if (match(TokenType::Greater)) {
        op = ComparisonOperator::Greater;
    } else if (match(TokenType::Less)) {
        op = ComparisonOperator::Less;
    } else {
        throw std::runtime_error("expected comparison operator");
    }
    return {column.lexeme, op, parseValue()};
}

Value Parser::parseValue() {
    const auto token = advance();
    if (token.type == TokenType::Identifier && equalsIgnoreCase(token.lexeme, "NULL")) {
        return Value{};
    }
    if (token.type == TokenType::String) {
        return Value{token.lexeme};
    }
    if (token.type == TokenType::Number) {
        if (token.lexeme.find('.') != std::string::npos) {
            return Value{std::stod(token.lexeme)};
        }
        return Value{parseInt(token.lexeme)};
    }
    throw std::runtime_error("expected literal value");
}

} // namespace VertexDB
