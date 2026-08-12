#include "VertexDB/planner/rewriter.hpp"

#include "VertexDB/common/string_utils.hpp"

#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

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

[[nodiscard]] bool referencesCteInFromOrJoin(const Select &query, const CteEntry &cte) {
    if (equalsIgnoreCase(cte.name, query.table)) {
        return true;
    }
    for (const auto &join : query.joins) {
        if (equalsIgnoreCase(cte.name, join.table)) {
            return true;
        }
    }
    return false;
}

void appendRewrite(RewriteResult &result, RewriteResult &&part) {
    for (auto &note : part.notes) {
        result.notes.push_back(std::move(note));
    }
    for (auto &item : part.materialize) {
        result.materialize.push_back(std::move(item));
    }
}

[[nodiscard]] Select rewriteBody(const Select &body, RewriteResult &result) {
    Select bodySelect = body;
    if (!bodySelect.ctes.empty()) {
        auto bodyRewrite = rewriteSelect(bodySelect);
        bodySelect = std::move(bodyRewrite.query);
        appendRewrite(result, std::move(bodyRewrite));
    }
    return bodySelect;
}

} // namespace

RewriteResult rewriteSelect(const Select &query) {
    RewriteResult result;
    result.query = query;

    if (query.ctes.empty()) {
        result.query = stripCtes(std::move(result.query));
        return result;
    }

    std::vector<const CteEntry *> joinTargets;
    for (const auto &cte : query.ctes) {
        if (referencesCteInFromOrJoin(query, cte)) {
            joinTargets.push_back(&cte);
        }
    }

    // Outer JOIN against a CTE/derived alias: force materialize so body WHERE stays inside the
    // temp and is not AND-merged onto the joined outer result.
    if (!query.joins.empty() && !joinTargets.empty()) {
        Select remaining = query;
        remaining.ctes.clear();
        for (const auto *cte : joinTargets) {
            Select bodySelect = rewriteBody(*cte->body, result);
            result.notes.push_back("materialized CTE " + cte->name + " (join target)");
            CteEntry materializeEntry = *cte;
            materializeEntry.body = std::make_shared<Select>(std::move(bodySelect));
            if (cte->recursive && cte->recursiveArm) {
                materializeEntry.recursiveArm =
                    std::make_shared<Select>(rewriteBody(*cte->recursiveArm, result));
                result.notes.back() = "materialized recursive CTE " + cte->name + " (join target)";
            }
            result.materialize.push_back(std::move(materializeEntry));
        }
        result.query = stripCtes(std::move(remaining));
        return result;
    }

    const CteEntry *matched = findCte(query, query.table);
    if (matched == nullptr) {
        // FROM is a base table; CTEs unused in FROM (may still appear only as documentation).
        result.query = stripCtes(std::move(result.query));
        result.notes.push_back("CTEs present but FROM references a base table");
        return result;
    }

    // Fully rewrite the CTE/derived body first (nested derived tables become synthetic CTEs).
    Select bodySelect = rewriteBody(*matched->body, result);

    if (matched->recursive || !bodySelect.setOps.empty()) {
        if (matched->recursive) {
            result.notes.push_back("materialized recursive CTE " + matched->name);
        } else {
            result.notes.push_back("materialized CTE " + matched->name + " (set operation)");
        }
        Select remaining = query;
        remaining.ctes.clear();
        for (const auto &cte : query.ctes) {
            if (!equalsIgnoreCase(cte.name, matched->name)) {
                remaining.ctes.push_back(cte);
            }
        }
        CteEntry materializeEntry = *matched;
        materializeEntry.body = std::make_shared<Select>(std::move(bodySelect));
        if (matched->recursive && matched->recursiveArm) {
            materializeEntry.recursiveArm =
                std::make_shared<Select>(rewriteBody(*matched->recursiveArm, result));
        }
        result.materialize.push_back(std::move(materializeEntry));
        if (!remaining.ctes.empty()) {
            auto recursive = rewriteSelect(remaining);
            result.query = std::move(recursive.query);
            appendRewrite(result, std::move(recursive));
            return result;
        }
        result.query = stripCtes(std::move(remaining));
        return result;
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
        CteEntry materializeEntry = *matched;
        materializeEntry.body = std::make_shared<Select>(std::move(bodySelect));
        result.materialize.push_back(std::move(materializeEntry));
        if (!remaining.ctes.empty()) {
            auto recursive = rewriteSelect(remaining);
            result.query = std::move(recursive.query);
            appendRewrite(result, std::move(recursive));
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
        result.query = std::move(recursive.query);
        appendRewrite(result, std::move(recursive));
        return result;
    }

    result.query = std::move(inlined);
    return result;
}

} // namespace VertexDB
