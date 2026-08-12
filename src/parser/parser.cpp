#include "VertexDB/parser/parser.hpp"

#include "VertexDB/common/string_utils.hpp"
#include "VertexDB/parser/parse_error.hpp"
#include "VertexDB/parser/tokenizer.hpp"
#include "parse_utils.hpp"

#include <string>

namespace VertexDB {
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
        error("expected DATABASE, TABLE, or INDEX after CREATE");
    }
    if (match(TokenType::Identifier, "DROP")) {
        if (match(TokenType::Identifier, "INDEX")) {
            return finish(parseDropIndex());
        }
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
    if (match(TokenType::Identifier, "WITH")) {
        return finish(parseWithSelect());
    }
    if (match(TokenType::Identifier, "SELECT")) {
        return finish(parseSelectAfterSelectKeyword());
    }
    if (match(TokenType::Identifier, "EXPLAIN")) {
        return finish(parseExplain());
    }
    if (match(TokenType::Identifier, "ANALYZE")) {
        return finish(parseAnalyze());
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

    error("unsupported SQL command");
}

const Token &Parser::peek() const {
    if (current_ >= tokens_.size()) {
        throw ParseError("parser read past end of token stream", 1, 1, 0);
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
        if (lexeme.empty()) {
            error("unexpected token");
        }
        error("expected '" + std::string{lexeme} + "'");
    }
}

void Parser::expectStatementEnd() {
    (void)match(TokenType::Semicolon);
    if (peek().type != TokenType::End) {
        error("unexpected trailing token");
    }
}

[[noreturn]] void Parser::error(std::string_view message) const {
    const Token &token = peek();
    throw ParseError(message, token.line, token.column, token.offset);
}

} // namespace VertexDB
