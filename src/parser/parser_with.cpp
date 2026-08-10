#include "VertexDB/parser/parser.hpp"

#include "VertexDB/common/string_utils.hpp"
#include "parse_utils.hpp"

#include <memory>
#include <stdexcept>

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
            body = parseSelect();
        }
        std::shared_ptr<Select> recursiveArm;
        bool recursive = false;
        if (match(TokenType::Identifier, "UNION")) {
            if (!match(TokenType::Identifier, "ALL")) {
                throw std::runtime_error("WITH RECURSIVE requires UNION ALL");
            }
            if (!recursiveWith) {
                throw std::runtime_error("UNION ALL in CTE requires WITH RECURSIVE");
            }
            Select arm;
            if (match(TokenType::Identifier, "WITH")) {
                throw std::runtime_error("WITH inside recursive arm is not supported");
            }
            arm = parseSelect();
            recursiveArm = std::make_shared<Select>(std::move(arm));
            recursive = true;
            ++recursiveCount;
            if (recursiveCount > 1) {
                throw std::runtime_error("only one recursive CTE is supported");
            }
            auto countSelfRefs = [](const Select &select, std::string_view cteName) {
                std::size_t count = 0;
                if (equalsIgnoreCase(select.table, cteName)) {
                    ++count;
                }
                for (const auto &join : select.joins) {
                    if (equalsIgnoreCase(join.table, cteName)) {
                        ++count;
                    }
                }
                return count;
            };
            if (countSelfRefs(body, name.lexeme) != 0) {
                throw std::runtime_error("recursive CTE anchor must not reference itself");
            }
            if (countSelfRefs(*recursiveArm, name.lexeme) != 1) {
                throw std::runtime_error(
                    "recursive CTE arm must reference the CTE name exactly once");
            }
        }
        expect(TokenType::RightParen);
        ctes.push_back(CteEntry{name.lexeme, std::make_shared<Select>(std::move(body)), mode,
                                recursive, std::move(recursiveArm)});
    } while (match(TokenType::Comma));

    if (recursiveWith && recursiveCount == 0) {
        throw std::runtime_error("WITH RECURSIVE requires a UNION ALL recursive CTE");
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
