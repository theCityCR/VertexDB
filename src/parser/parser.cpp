#include "VertexDB/parser/parser.hpp"

#include "VertexDB/common/string_utils.hpp"
#include "VertexDB/parser/tokenizer.hpp"
#include "parse_utils.hpp"

#include <stdexcept>

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
    if (match(TokenType::Identifier, "WITH")) {
        return finish(parseWithSelect());
    }
    if (match(TokenType::Identifier, "SELECT")) {
        return finish(parseSelectAfterSelectKeyword());
    }
    if (match(TokenType::Identifier, "EXPLAIN")) {
        return finish(parseExplain());
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

} // namespace VertexDB
