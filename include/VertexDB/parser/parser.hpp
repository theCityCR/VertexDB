#pragma once

// SQL Parser type. Grammar TUs: parser.cpp (dispatch), parser_ddl.cpp,
// parser_dml.cpp, parser_predicate.cpp. AST catalog: ast.hpp.

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
    [[nodiscard]] Select parseSelectAfterSelectKeyword();
    [[nodiscard]] Select parseWithSelect();
    // depth 0 = top-level WITH; depth 1 = one nested WITH inside a CTE body (max supported).
    [[nodiscard]] Select parseWithSelectAtDepth(int depth);
    [[nodiscard]] Update parseUpdate();
    [[nodiscard]] Delete parseDelete();
    [[nodiscard]] PrepareStatement parsePrepare();
    [[nodiscard]] ExecutePrepared parseExecutePrepared();
    [[nodiscard]] ExplainQuery parseExplain();
    [[nodiscard]] Analyze parseAnalyze();
    [[nodiscard]] Predicate parsePredicate();
    [[nodiscard]] Predicate parseOrPredicate();
    [[nodiscard]] Predicate parseAndPredicate();
    [[nodiscard]] Predicate parsePrimaryPredicate();
    [[nodiscard]] Predicate parseComparisonPredicate();
    [[nodiscard]] Predicate parseExistsPredicate();
    [[nodiscard]] IndexExpression parseIndexExpression();
    [[nodiscard]] Value parseValue();
    [[nodiscard]] Select parseSubquerySelect(bool allowOuterRefs);
    void markOuterRefs(Select &select, std::string_view innerTable, bool nestedUnderCorrelated);
    void markOuterRefs(Predicate &predicate, std::string_view innerTable,
                       bool nestedUnderCorrelated);

    std::span<const Token> tokens_;
    std::size_t current_{0};
    // Immediate FROM table of the SELECT whose WHERE/predicate is being parsed.
    std::string currentFromTable_;
    // Outer FROM tables while parsing nested subqueries (immediate outer is back()).
    std::vector<std::string> outerTableStack_;
    // Positional `?` parameter indices assigned while parsing prepared SQL.
    std::size_t nextParameterIndex_{0};

    // Max outer FROM frames while parsing nested IN/EXISTS (two-level correlation).
    static constexpr std::size_t kMaxOuterCorrelationDepth = 2;
};

} // namespace VertexDB
