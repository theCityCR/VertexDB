#include "VertexDB/parser/parser.hpp"

#include "VertexDB/common/string_utils.hpp"
#include "VertexDB/parser/recursive_cte.hpp"
#include "parse_utils.hpp"

#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace VertexDB {

Select Parser::parseWithSelect() { return parseWithSelectAtDepth(0); }

Select Parser::parseWithSelectAtDepth(int depth) {
    const bool recursiveWith = match(TokenType::Identifier, "RECURSIVE");
    std::vector<CteEntry> ctes;
    std::size_t recursiveCount = 0;
    do {
        const auto name = advance();
        if (name.type != TokenType::Identifier) {
            throw std::runtime_error("expected CTE name");
        }
        expect(TokenType::Identifier, "AS");
        MaterializeMode mode = MaterializeMode::DefaultInline;
        bool recursiveBindAccumulator = false;
        if (match(TokenType::Identifier, "NOT")) {
            expect(TokenType::Identifier, "MATERIALIZED");
            mode = MaterializeMode::NotMaterialized;
        } else if (match(TokenType::Identifier, "MATERIALIZED")) {
            mode = MaterializeMode::Materialized;
        } else if (match(TokenType::Identifier, "ACCUMULATOR")) {
            recursiveBindAccumulator = true;
        }
        expect(TokenType::LeftParen);
        Select body;
        if (match(TokenType::Identifier, "WITH")) {
            if (depth >= kMaxNestedWithDepth) {
                throw std::runtime_error("nested WITH exceeds maximum depth");
            }
            body = parseWithSelectAtDepth(depth + 1);
        } else {
            expect(TokenType::Identifier, "SELECT");
            body = parseSelectAfterSelectKeyword();
        }

        std::shared_ptr<Select> recursiveArm;
        bool recursive = false;
        bool recursiveDistinct = false;

        // Peel a single trailing UNION [ALL] into a recursive CTE when WITH RECURSIVE.
        // Non-recursive CTEs may keep a full set-op chain as the body.
        if (recursiveWith && body.setOps.size() == 1 &&
            (body.setOps[0].op == SetOpKind::Union || body.setOps[0].op == SetOpKind::UnionAll) &&
            body.setOps[0].select) {
            if (body.orderBy || body.limit) {
                throw std::runtime_error(
                    "ORDER BY / LIMIT on recursive CTE bodies is not supported");
            }
            recursiveArm = body.setOps[0].select;
            recursiveDistinct = body.setOps[0].op == SetOpKind::Union;
            body.setOps.clear();
            recursive = true;
            ++recursiveCount;
            if (!recursiveArm->setOps.empty()) {
                throw std::runtime_error("recursive CTE arm must be a single SELECT");
            }
        } else if (recursiveWith && !body.setOps.empty()) {
            throw std::runtime_error(
                "WITH RECURSIVE requires a single UNION [ALL] between anchor and recursive arm");
        } else if (!recursive && countTableRefs(body, name.lexeme) != 0) {
            throw std::runtime_error("CTE self-reference requires WITH RECURSIVE");
        }

        if (recursiveBindAccumulator && !recursive) {
            throw std::runtime_error("AS ACCUMULATOR requires a recursive UNION [ALL] CTE");
        }

        expect(TokenType::RightParen);
        ctes.push_back(CteEntry{name.lexeme, std::make_shared<Select>(std::move(body)), mode,
                                recursive, recursiveDistinct, recursiveBindAccumulator,
                                std::move(recursiveArm)});
    } while (match(TokenType::Comma));

    if (recursiveWith && recursiveCount == 0) {
        throw std::runtime_error("WITH RECURSIVE requires a UNION [ALL] recursive CTE");
    }
    if (recursiveWith) {
        validateRecursiveCtes(ctes);
    }

    expect(TokenType::Identifier, "SELECT");
    auto query = parseSelectAfterSelectKeyword();
    // Derived-table CTEs are closest to FROM; append WITH CTEs after so a colliding FROM alias
    // prefers the derived table, while WITH names remain available for bodies/siblings.
    if (!query.ctes.empty()) {
        auto combined = std::move(query.ctes);
        for (auto &cte : ctes) {
            combined.push_back(std::move(cte));
        }
        query.ctes = std::move(combined);
    } else {
        query.ctes = std::move(ctes);
    }
    return query;
}

} // namespace VertexDB
