#include "VertexDB/parser/parser.hpp"

#include "VertexDB/common/string_utils.hpp"
#include "parse_utils.hpp"

#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace VertexDB {
namespace {

[[nodiscard]] std::size_t countTableRefs(const Select &select, std::string_view tableName) {
    std::size_t count = 0;
    if (equalsIgnoreCase(select.table, tableName)) {
        ++count;
    }
    for (const auto &join : select.joins) {
        if (equalsIgnoreCase(join.table, tableName)) {
            ++count;
        }
    }
    for (const auto &arm : select.setOps) {
        if (arm.select) {
            count += countTableRefs(*arm.select, tableName);
        }
    }
    return count;
}

[[nodiscard]] bool selectReferencesTable(const Select &select, std::string_view tableName) {
    return countTableRefs(select, tableName) > 0;
}

[[nodiscard]] bool cteReferencesName(const CteEntry &cte, std::string_view tableName) {
    if (cte.body && selectReferencesTable(*cte.body, tableName)) {
        return true;
    }
    if (cte.recursiveArm && selectReferencesTable(*cte.recursiveArm, tableName)) {
        return true;
    }
    return false;
}

// Reject mutual recursion: a cycle among recursive CTE names via FROM/JOIN refs.
void rejectMutualRecursiveCtes(const std::vector<CteEntry> &ctes) {
    std::vector<std::size_t> recursiveIndexes;
    for (std::size_t i = 0; i < ctes.size(); ++i) {
        if (ctes[i].recursive) {
            recursiveIndexes.push_back(i);
        }
    }
    if (recursiveIndexes.size() < 2) {
        return;
    }

    std::unordered_map<std::string, std::vector<std::string>> edges;
    for (const std::size_t i : recursiveIndexes) {
        const auto &from = ctes[i];
        for (const std::size_t j : recursiveIndexes) {
            if (i == j) {
                continue;
            }
            const auto &to = ctes[j];
            if (cteReferencesName(from, to.name)) {
                edges[from.name].push_back(to.name);
            }
        }
    }

    std::unordered_set<std::string> visiting;
    std::unordered_set<std::string> visited;
    const auto &dfs = [&](auto &&self, const std::string &node) -> bool {
        if (visiting.contains(node)) {
            return true;
        }
        if (visited.contains(node)) {
            return false;
        }
        visiting.insert(node);
        for (const auto &next : edges[node]) {
            if (self(self, next)) {
                return true;
            }
        }
        visiting.erase(node);
        visited.insert(node);
        return false;
    };

    for (const std::size_t i : recursiveIndexes) {
        if (dfs(dfs, ctes[i].name)) {
            throw std::runtime_error("mutual recursion among WITH RECURSIVE CTEs is not supported");
        }
    }
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
            if (!recursiveArm->setOps.empty()) {
                throw std::runtime_error("recursive CTE arm must be a single SELECT");
            }
            if (countTableRefs(body, name.lexeme) != 0) {
                throw std::runtime_error("recursive CTE anchor must not reference itself");
            }
            if (countTableRefs(*recursiveArm, name.lexeme) != 1) {
                throw std::runtime_error(
                    "recursive CTE arm must reference the CTE name exactly once");
            }
        } else if (recursiveWith && !body.setOps.empty()) {
            throw std::runtime_error(
                "WITH RECURSIVE requires a single UNION [ALL] between anchor and recursive arm");
        } else if (!recursive && countTableRefs(body, name.lexeme) != 0) {
            throw std::runtime_error("CTE self-reference requires WITH RECURSIVE");
        }

        expect(TokenType::RightParen);
        ctes.push_back(CteEntry{name.lexeme, std::make_shared<Select>(std::move(body)), mode,
                                recursive, recursiveDistinct, std::move(recursiveArm)});
    } while (match(TokenType::Comma));

    if (recursiveWith && recursiveCount == 0) {
        throw std::runtime_error("WITH RECURSIVE requires a UNION [ALL] recursive CTE");
    }
    rejectMutualRecursiveCtes(ctes);

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
