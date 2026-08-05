#pragma once

#include "VertexDB/parser/ast.hpp"
#include "VertexDB/parser/token.hpp"

#include <span>
#include <string_view>

namespace VertexDB {

class Parser {
  public:
    [[nodiscard]] Query parse(std::string_view sql);
    [[nodiscard]] Query parse(std::span<const Token> tokens);

  private:
    [[nodiscard]] const Token &peek() const;
    [[nodiscard]] const Token &advance();
    [[nodiscard]] bool match(TokenType type, std::string_view lexeme = {});
    void expect(TokenType type, std::string_view lexeme = {});
    void expectStatementEnd();

    [[nodiscard]] CreateDatabase parseCreateDatabase();
    [[nodiscard]] CreateTable parseCreateTable();
    [[nodiscard]] DropTable parseDropTable();
    [[nodiscard]] RenameTable parseRenameTable();
    [[nodiscard]] CreateIndex parseCreateIndex();
    [[nodiscard]] Insert parseInsert();
    [[nodiscard]] Select parseSelect();
    [[nodiscard]] Update parseUpdate();
    [[nodiscard]] Delete parseDelete();
    [[nodiscard]] PrepareStatement parsePrepare();
    [[nodiscard]] ExecutePrepared parseExecutePrepared();
    [[nodiscard]] Predicate parsePredicate();
    [[nodiscard]] Predicate parseOrPredicate();
    [[nodiscard]] Predicate parseAndPredicate();
    [[nodiscard]] Predicate parsePrimaryPredicate();
    [[nodiscard]] Predicate parseComparisonPredicate();
    [[nodiscard]] Value parseValue();

    std::span<const Token> tokens_;
    std::size_t current_{0};
};

} // namespace VertexDB
