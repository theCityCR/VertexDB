#include "query_planner_access.hpp"

#include <algorithm>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace VertexDB {
namespace planner_detail {

bool tryPlanCompositeHashEq(const std::vector<const Predicate *> &conjuncts,
                            const RelationStats &stats, const IndexCatalogView &indexes,
                            double bestCost, QueryPlan &plan) {
    // Map simple column equality conjuncts (no expression / join rhs) to their predicates.
    struct EqLit {
        const Predicate *predicate{nullptr};
        Value value;
    };
    std::unordered_map<std::string, EqLit> equalityByColumn;
    for (const Predicate *pred : conjuncts) {
        const auto *comparison = std::get_if<ComparisonPred>(pred);
        if (comparison == nullptr || comparison->rhsColumn || comparison->expression ||
            comparison->op != ComparisonOperator::Equal) {
            continue;
        }
        // First conjunct wins if the same column appears twice (residual keeps the rest).
        equalityByColumn.emplace(comparison->column, EqLit{pred, comparison->value});
    }
    if (equalityByColumn.size() < 2) {
        return false;
    }

    const auto composites = indexes.compositeIndexColumnLists();
    if (composites.empty()) {
        return false;
    }

    struct Candidate {
        std::vector<std::string> columns;
        std::vector<Value> parts;
        std::vector<const Predicate *> absorbed;
        double cost{0.0};
    };
    std::optional<Candidate> best;

    for (const auto &columns : composites) {
        if (columns.size() < 2 || !indexes.hasIndex(columns)) {
            continue;
        }
        Candidate candidate;
        candidate.columns = columns;
        candidate.parts.reserve(columns.size());
        candidate.absorbed.reserve(columns.size());
        bool covered = true;
        for (const auto &column : columns) {
            const auto it = equalityByColumn.find(column);
            if (it == equalityByColumn.end()) {
                covered = false;
                break;
            }
            candidate.parts.push_back(it->second.value);
            candidate.absorbed.push_back(it->second.predicate);
        }
        if (!covered) {
            continue;
        }

        const std::size_t N = std::max<std::size_t>(stats.rowCount(), 1);
        std::size_t distinct = indexes.indexDistinctCount(columns).value_or(0);
        if (distinct == 0) {
            // Fall back to independence over per-column distincts when composite stats are cold.
            distinct = 1;
            for (const auto &column : columns) {
                const std::size_t part = distinctOrOne(stats, indexes, column);
                if (distinct > N / std::max<std::size_t>(part, 1)) {
                    distinct = N;
                    break;
                }
                distinct *= part;
            }
            distinct = std::min(distinct, N);
        }
        candidate.cost = averageRowsPerKey(N, distinct);
        // Prefer composite HashEq over FullScan / equal-cost single-conjunct paths (`<=`),
        // so multi-equality AND can use the composite even when fanout ties a scan.
        if (candidate.cost > bestCost) {
            continue;
        }
        if (!best || candidate.cost < best->cost ||
            (candidate.cost == best->cost && candidate.columns.size() > best->columns.size())) {
            best = std::move(candidate);
        }
    }

    if (!best) {
        return false;
    }

    HashEqPlan hashEq;
    hashEq.indexColumn = best->columns.front();
    hashEq.indexColumns = best->columns;
    hashEq.indexValue = Value::composite(std::move(best->parts));
    plan.path = std::move(hashEq);
    plan.estimates.estimatedCost = best->cost;
    plan.estimates.estimatedRows = rowsFromCost(best->cost, stats.rowCount());

    std::ostringstream labels;
    for (std::size_t i = 0; i < best->columns.size(); ++i) {
        if (i > 0) {
            labels << ", ";
        }
        labels << best->columns[i];
    }
    plan.estimates.explanation = "hash index equality lookup on " + labels.str();
    plan.estimates.notes.push_back("composite index equality for multi-column AND");

    std::vector<const Predicate *> residualConjuncts;
    residualConjuncts.reserve(conjuncts.size());
    for (const Predicate *pred : conjuncts) {
        const bool absorbed =
            std::any_of(best->absorbed.begin(), best->absorbed.end(),
                        [pred](const Predicate *taken) { return taken == pred; });
        if (!absorbed) {
            residualConjuncts.push_back(pred);
        }
    }
    plan.estimates.residual = buildAndTree(residualConjuncts);
    if (plan.estimates.residual) {
        plan.estimates.notes.push_back("residual filter applied after index lookup");
    }
    return true;
}

} // namespace planner_detail
} // namespace VertexDB
