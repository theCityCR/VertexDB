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
    return makeAnd(std::move(*left), std::move(*right));
}

Select stripCtes(Select query) {
    query.ctes.clear();
    return query;
}

[[nodiscard]] const CteEntry *findCte(const Select &query, std::string_view table) {
    for (const auto &cte : query.ctes) {
        if (equalsIgnoreCase(cte.name, table)) {
            return &cte;
        }
    }
    return nullptr;
}

} // namespace

RewriteResult rewriteSelect(const Select &query) {
    RewriteResult result;
    result.query = query;

    if (query.ctes.empty()) {
        result.query = stripCtes(std::move(result.query));
        return result;
    }

    const CteEntry *matched = findCte(query, query.table);
    if (matched == nullptr) {
        // FROM is a base table; CTEs unused in FROM (may still appear only as documentation).
        result.query = stripCtes(std::move(result.query));
        result.notes.push_back("CTEs present but FROM references a base table");
        return result;
    }

    if (!query.joins.empty()) {
        // Body WHERE is AND-merged onto the joined result; that would mis-scope filters and can
        // make columns ambiguous. Require JOIN to live inside the CTE/derived body instead.
        throw std::runtime_error("JOIN with CTE is not supported in this version");
    }

    // Fully rewrite the CTE/derived body first (nested derived tables become synthetic CTEs).
    Select bodySelect = *matched->body;
    if (!bodySelect.ctes.empty()) {
        auto bodyRewrite = rewriteSelect(bodySelect);
        for (auto &note : bodyRewrite.notes) {
            result.notes.push_back(std::move(note));
        }
        for (auto &item : bodyRewrite.materialize) {
            result.materialize.push_back(std::move(item));
        }
        bodySelect = std::move(bodyRewrite.query);
    }

    if (matched->materializeMode == MaterializeMode::Materialized) {
        result.notes.push_back("materialized CTE " + matched->name);
        Select remaining = query;
        remaining.ctes.clear();
        for (const auto &cte : query.ctes) {
            if (!equalsIgnoreCase(cte.name, matched->name)) {
                remaining.ctes.push_back(cte);
            }
        }
        result.materialize.emplace_back(matched->name, std::move(bodySelect));
        if (!remaining.ctes.empty()) {
            auto recursive = rewriteSelect(remaining);
            for (auto &note : recursive.notes) {
                result.notes.push_back(std::move(note));
            }
            for (auto &item : recursive.materialize) {
                result.materialize.push_back(std::move(item));
            }
            result.query = std::move(recursive.query);
            return result;
        }
        result.query = stripCtes(std::move(remaining));
        return result;
    }

    // DefaultInline / NotMaterialized: always inline (today's behavior).
    Select inlined = std::move(bodySelect);
    // Outer projection wins when not "*"; CTE projection restricts available columns for "*".
    if (!isStarProjection(query.columns)) {
        inlined.columns = query.columns;
    } else if (!isStarProjection(inlined.columns)) {
        // Keep CTE projection when outer asks for *.
    } else {
        inlined.columns = query.columns;
    }

    inlined.where = andPredicates(std::move(inlined.where), query.where);
    if (!query.groupBy.empty()) {
        inlined.groupBy = query.groupBy;
    }
    inlined.orderBy = query.orderBy ? query.orderBy : inlined.orderBy;
    if (query.limit) {
        inlined.limit = query.limit;
    }
    inlined.ctes.clear();
    // Preserve JOINs that lived inside the CTE/derived body.

    result.notes.push_back("inlined CTE " + matched->name);
    // Recursively inline if the CTE body still referenced another CTE name from the same WITH.
    // Rebuild a Select that carries remaining CTEs for nested references.
    if (query.ctes.size() > 1) {
        Select nested = inlined;
        for (const auto &cte : query.ctes) {
            if (!equalsIgnoreCase(cte.name, matched->name)) {
                nested.ctes.push_back(cte);
            }
        }
        auto recursive = rewriteSelect(nested);
        for (auto &note : recursive.notes) {
            result.notes.push_back(std::move(note));
        }
        for (auto &item : recursive.materialize) {
            result.materialize.push_back(std::move(item));
        }
        result.query = std::move(recursive.query);
        return result;
    }

    result.query = std::move(inlined);
    return result;
}

} // namespace VertexDB
