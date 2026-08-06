#include "VertexDB/planner/rewriter.hpp"

#include "VertexDB/common/string_utils.hpp"

#include <stdexcept>
#include <utility>

namespace VertexDB {
namespace {

std::optional<Predicate> andPredicates(std::optional<Predicate> left,
                                       std::optional<Predicate> right) {
    if (!left) {
        return right;
    }
    if (!right) {
        return left;
    }
    return Predicate{Predicate::Kind::And, std::make_shared<Predicate>(std::move(*left)),
                     std::make_shared<Predicate>(std::move(*right))};
}

Select stripCtes(Select query) {
    query.ctes.clear();
    return query;
}

} // namespace

RewriteResult rewriteSelect(const Select &query) {
    RewriteResult result;
    result.query = query;

    if (query.ctes.empty()) {
        result.query = stripCtes(std::move(result.query));
        return result;
    }

    const auto *matched = static_cast<const Select *>(nullptr);
    std::string matchedName;
    for (const auto &[name, body] : query.ctes) {
        if (equalsIgnoreCase(name, query.table)) {
            matched = body.get();
            matchedName = name;
            break;
        }
    }

    if (matched == nullptr) {
        // FROM is a base table; CTEs unused in FROM (may still appear only as documentation).
        result.query = stripCtes(std::move(result.query));
        result.notes.push_back("CTEs present but FROM references a base table");
        return result;
    }

    if (query.join) {
        // Body WHERE is AND-merged onto the joined result; that would mis-scope filters and can
        // make columns ambiguous. Require JOIN to live inside the CTE/derived body instead.
        throw std::runtime_error("JOIN with CTE is not supported in this version");
    }

    // Fully rewrite the CTE/derived body first (nested derived tables become synthetic CTEs).
    Select bodySelect = *matched;
    if (!bodySelect.ctes.empty()) {
        auto bodyRewrite = rewriteSelect(bodySelect);
        for (auto &note : bodyRewrite.notes) {
            result.notes.push_back(std::move(note));
        }
        bodySelect = std::move(bodyRewrite.query);
    }

    Select inlined = std::move(bodySelect);
    // Outer projection wins when not "*"; CTE projection restricts available columns for "*".
    if (!(query.columns.size() == 1 && query.columns.front() == "*")) {
        inlined.columns = query.columns;
    } else if (!(inlined.columns.size() == 1 && inlined.columns.front() == "*")) {
        // Keep CTE projection when outer asks for *.
    } else {
        inlined.columns = query.columns;
    }

    inlined.where = andPredicates(std::move(inlined.where), query.where);
    inlined.orderBy = query.orderBy ? query.orderBy : inlined.orderBy;
    if (query.limit) {
        inlined.limit = query.limit;
    }
    inlined.ctes.clear();
    // Preserve a JOIN that lived inside the CTE/derived body.

    result.notes.push_back("inlined CTE " + matchedName);
    // Recursively inline if the CTE body still referenced another CTE name from the same WITH.
    // Rebuild a Select that carries remaining CTEs for nested references.
    if (query.ctes.size() > 1) {
        Select nested = inlined;
        for (const auto &[name, body] : query.ctes) {
            if (!equalsIgnoreCase(name, matchedName)) {
                nested.ctes.emplace_back(name, body);
            }
        }
        auto recursive = rewriteSelect(nested);
        for (auto &note : recursive.notes) {
            result.notes.push_back(std::move(note));
        }
        result.query = std::move(recursive.query);
        return result;
    }

    result.query = std::move(inlined);
    return result;
}

} // namespace VertexDB
