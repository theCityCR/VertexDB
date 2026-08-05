#include "VertexDB/planner/rewriter.hpp"

#include <cctype>
#include <stdexcept>
#include <utility>

namespace VertexDB {
namespace {

bool equalsIgnoreCase(std::string_view lhs, std::string_view rhs) {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        const auto left = static_cast<unsigned char>(lhs[i]);
        const auto right = static_cast<unsigned char>(rhs[i]);
        if (std::toupper(left) != std::toupper(right)) {
            return false;
        }
    }
    return true;
}

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

    if (query.join) {
        throw std::runtime_error("JOIN with CTE is not supported in this version");
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

    if (matched->join) {
        throw std::runtime_error("JOIN inside CTE is not supported in this version");
    }
    if (!matched->ctes.empty()) {
        throw std::runtime_error("nested WITH inside CTE is not supported in this version");
    }

    Select inlined = *matched;
    // Outer projection wins when not "*"; CTE projection restricts available columns for "*".
    if (!(query.columns.size() == 1 && query.columns.front() == "*")) {
        inlined.columns = query.columns;
    } else if (!(inlined.columns.size() == 1 && inlined.columns.front() == "*")) {
        // Keep CTE projection when outer asks for *.
    } else {
        inlined.columns = query.columns;
    }

    inlined.where = andPredicates(inlined.where, query.where);
    inlined.orderBy = query.orderBy ? query.orderBy : inlined.orderBy;
    if (query.limit) {
        inlined.limit = query.limit;
    }
    inlined.ctes.clear();
    inlined.join = std::nullopt;

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
