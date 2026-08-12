#include "VertexDB/parser/parser.hpp"

#include "VertexDB/common/string_utils.hpp"
#include "parse_utils.hpp"

#include <memory>
#include <stdexcept>

namespace VertexDB {
namespace {

[[nodiscard]] std::size_t countSelfRefs(const Select &select, std::string_view cteName) {
    std::size_t count = 0;
    if (equalsIgnoreCase(select.table, cteName)) {
        ++count;
    }
    for (const auto &join : select.joins) {
        if (equalsIgnoreCase(join.table, cteName)) {
            ++count;
        }
    }
    for (const auto &arm : select.setOps) {
        if (arm.select) {
            count += countSelfRefs(*arm.select, cteName);
        }
    }
    return count;
}

} // namespace

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
        if (match(TokenType::Identifier, "NOT")) {
            expect(TokenType::Identifier, "MATERIALIZED");
            mode = MaterializeMode::NotMaterialized;
        } else if (match(TokenType::Identifier, "MATERIALIZED")) {
            mode = MaterializeMode::Materialized;
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
            if (recursiveCount > 1) {
                throw std::runtime_error("only one recursive CTE is supported");
            }
            if (!recursiveArm->setOps.empty()) {
                throw std::runtime_error("recursive CTE arm must be a single SELECT");
            }
            if (countSelfRefs(body, name.lexeme) != 0) {
                throw std::runtime_error("recursive CTE anchor must not reference itself");
            }
            if (countSelfRefs(*recursiveArm, name.lexeme) != 1) {
                throw std::runtime_error(
                    "recursive CTE arm must reference the CTE name exactly once");
            }
        } else if (recursiveWith && !body.setOps.empty()) {
            throw std::runtime_error(
                "WITH RECURSIVE requires a single UNION [ALL] between anchor and recursive arm");
        } else if (!recursive && countSelfRefs(body, name.lexeme) != 0) {
            throw std::runtime_error("CTE self-reference requires WITH RECURSIVE");
        }

        expect(TokenType::RightParen);
        ctes.push_back(CteEntry{name.lexeme, std::make_shared<Select>(std::move(body)), mode,
                                recursive, recursiveDistinct, std::move(recursiveArm)});
    } while (match(TokenType::Comma));

    if (recursiveWith && recursiveCount == 0) {
        throw std::runtime_error("WITH RECURSIVE requires a UNION [ALL] recursive CTE");
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
