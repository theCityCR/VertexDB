#include "planner_detail.hpp"

#include "VertexDB/common/index_expression.hpp"
#include "VertexDB/common/string_pattern.hpp"
#include "VertexDB/storage/histogram.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace VertexDB {
namespace planner_detail {

void collectAndConjuncts(const Predicate &predicate, std::vector<const Predicate *> &out) {
    if (const auto *andPred = std::get_if<AndPred>(&predicate)) {
        collectAndConjuncts(*andPred->left, out);
        collectAndConjuncts(*andPred->right, out);
        return;
    }
    out.push_back(&predicate);
}

void collectOrDisjuncts(const Predicate &predicate, std::vector<const Predicate *> &out) {
    if (const auto *orPred = std::get_if<OrPred>(&predicate)) {
        collectOrDisjuncts(*orPred->left, out);
        collectOrDisjuncts(*orPred->right, out);
        return;
    }
    out.push_back(&predicate);
}

std::optional<Predicate> buildAndTree(const std::vector<const Predicate *> &conjuncts) {
    if (conjuncts.empty()) {
        return std::nullopt;
    }
    Predicate tree = *conjuncts.front();
    for (std::size_t i = 1; i < conjuncts.size(); ++i) {
        tree = makeAnd(std::move(tree), *conjuncts[i]);
    }
    return tree;
}

std::optional<Predicate> buildOrTree(const std::vector<const Predicate *> &disjuncts) {
    if (disjuncts.empty()) {
        return std::nullopt;
    }
    Predicate tree = *disjuncts.front();
    for (std::size_t i = 1; i < disjuncts.size(); ++i) {
        tree = makeOr(std::move(tree), *disjuncts[i]);
    }
    return tree;
}

double averageRowsPerKey(std::size_t rowCount, std::size_t distinctKeys) {
    if (rowCount == 0) {
        return 1.0;
    }
    const auto distinct = std::max<std::size_t>(distinctKeys, 1);
    return static_cast<double>(rowCount) / static_cast<double>(distinct);
}

std::size_t distinctOrOne(const RelationStats &stats, const IndexCatalogView &indexes,
                          std::string_view column) {
    if (const auto distinct = indexes.indexDistinctCount(column)) {
        return std::max<std::size_t>(*distinct, 1);
    }
    if (const auto histogram = stats.columnHistogram(column)) {
        return std::max<std::size_t>(static_cast<std::size_t>(histogram->distinctCount), 1);
    }
    return 1;
}

std::size_t distinctOrOne(const IndexCatalogView &indexes, const IndexExpression &expression) {
    if (const auto distinct = indexes.indexDistinctCount(expression)) {
        return std::max<std::size_t>(*distinct, 1);
    }
    return 1;
}

double rangeCost(const RelationStats &stats, std::string_view column, ComparisonOperator op,
                 const Value &value, std::size_t rowCount) {
    if (const auto histogram = stats.columnHistogram(column)) {
        const double selectivity = histogramRangeSelectivity(*histogram, op, value);
        return std::max(selectivity * static_cast<double>(std::max<std::size_t>(rowCount, 1)), 1.0);
    }
    return std::max(static_cast<double>(rowCount) / 3.0, 1.0);
}

double inCost(const RelationStats &stats, const IndexCatalogView &indexes, std::string_view column,
              const std::vector<Value> &values, std::size_t rowCount) {
    if (const auto histogram = stats.columnHistogram(column)) {
        const double selectivity = histogramInSelectivity(*histogram, values);
        return std::max(selectivity * static_cast<double>(std::max<std::size_t>(rowCount, 1)), 1.0);
    }
    return static_cast<double>(values.size()) *
           averageRowsPerKey(rowCount, distinctOrOne(stats, indexes, column));
}

bool isIndexableComparison(const Predicate &predicate, const RelationStats &stats,
                           const IndexCatalogView &indexes, AccessPath &path, double &cost,
                           std::size_t rowCount) {
    const auto *comparison = std::get_if<ComparisonPred>(&predicate);
    if (comparison == nullptr || comparison->rhsColumn) {
        return false;
    }
    if (comparison->expression) {
        if (!indexes.hasExpressionIndex(*comparison->expression)) {
            return false;
        }
        if (comparison->op == ComparisonOperator::Equal) {
            path = AccessPath::HashEq;
            cost = averageRowsPerKey(rowCount, distinctOrOne(indexes, *comparison->expression));
            return true;
        }
        if (comparison->op == ComparisonOperator::Greater ||
            comparison->op == ComparisonOperator::Less) {
            path = AccessPath::OrderedRange;
            cost = std::max(static_cast<double>(rowCount) / 3.0, 1.0);
            return true;
        }
        return false;
    }
    if (comparison->op == ComparisonOperator::Equal && indexes.hasIndex(comparison->column)) {
        path = AccessPath::HashEq;
        cost = averageRowsPerKey(rowCount, distinctOrOne(stats, indexes, comparison->column));
        return true;
    }
    if ((comparison->op == ComparisonOperator::Greater ||
         comparison->op == ComparisonOperator::Less) &&
        indexes.hasIndex(comparison->column)) {
        path = AccessPath::OrderedRange;
        cost = rangeCost(stats, comparison->column, comparison->op, comparison->value, rowCount);
        return true;
    }
    return false;
}

bool isIndexableInList(const Predicate &predicate, const RelationStats &stats,
                       const IndexCatalogView &indexes, double &cost, std::size_t rowCount) {
    const auto *inList = std::get_if<InListPred>(&predicate);
    if (inList == nullptr || inList->inValues.empty()) {
        return false;
    }
    if (inList->expression) {
        if (!indexes.hasExpressionIndex(*inList->expression)) {
            return false;
        }
        cost = static_cast<double>(inList->inValues.size()) *
               averageRowsPerKey(rowCount, distinctOrOne(indexes, *inList->expression));
        return true;
    }
    if (!indexes.hasIndex(inList->column)) {
        return false;
    }
    cost = inCost(stats, indexes, inList->column, inList->inValues, rowCount);
    return true;
}

std::optional<Predicate> tryRewriteSameColumnEqualityOrToIn(const Predicate &predicate) {
    if (!std::holds_alternative<OrPred>(predicate)) {
        return std::nullopt;
    }
    std::vector<const Predicate *> disjuncts;
    collectOrDisjuncts(predicate, disjuncts);
    if (disjuncts.size() < 2) {
        return std::nullopt;
    }

    const ComparisonPred *first = nullptr;
    std::vector<Value> values;
    values.reserve(disjuncts.size());
    for (const auto *disjunct : disjuncts) {
        const auto *comparison = std::get_if<ComparisonPred>(disjunct);
        if (comparison == nullptr || comparison->rhsColumn ||
            comparison->op != ComparisonOperator::Equal) {
            return std::nullopt;
        }
        if (first == nullptr) {
            first = comparison;
        } else if (comparison->column != first->column ||
                   comparison->expression != first->expression) {
            return std::nullopt;
        }
        values.push_back(comparison->value);
    }

    InListPred inList;
    inList.column = first->column;
    inList.inValues = std::move(values);
    inList.expression = first->expression;
    inList.referencesOuter = first->referencesOuter;
    return Predicate{std::move(inList)};
}

bool isEqualityIndexProbe(const Predicate &predicate, const IndexCatalogView &indexes) {
    const auto *comparison = std::get_if<ComparisonPred>(&predicate);
    if (comparison == nullptr || comparison->rhsColumn) {
        return false;
    }
    if (comparison->op != ComparisonOperator::Equal) {
        return false;
    }
    if (comparison->expression) {
        return indexes.hasExpressionIndex(*comparison->expression);
    }
    return indexes.hasIndex(comparison->column);
}

IndexEqualityProbe makeEqualityProbe(const Predicate &predicate) {
    const auto &comparison = std::get<ComparisonPred>(predicate);
    IndexEqualityProbe probe;
    probe.column = comparison.column;
    probe.expression = comparison.expression;
    probe.value = comparison.value;
    return probe;
}

double equalityFanout(const Predicate &predicate, const RelationStats &stats,
                      const IndexCatalogView &indexes, std::size_t rowCount) {
    const auto &comparison = std::get<ComparisonPred>(predicate);
    if (comparison.expression) {
        return averageRowsPerKey(rowCount, distinctOrOne(indexes, *comparison.expression));
    }
    return averageRowsPerKey(rowCount, distinctOrOne(stats, indexes, comparison.column));
}

std::size_t rowsFromCost(double cost, std::size_t rowCount) {
    if (rowCount == 0) {
        return 0;
    }
    return std::min(rowCount, static_cast<std::size_t>(std::max(std::llround(cost), 1LL)));
}

std::string probeLabel(const IndexEqualityProbe &probe) {
    if (probe.expression) {
        return "(" + indexExpressionToString(*probe.expression) + ")";
    }
    return probe.column;
}

bool isIndexableLike(const Predicate &predicate, const IndexCatalogView &indexes, AccessPath &path,
                     double &cost, std::size_t rowCount,
                     std::optional<IntersectPlan> &trigramIntersect) {
    const auto *like = std::get_if<LikePred>(&predicate);
    if (like == nullptr) {
        return false;
    }
    trigramIntersect.reset();

    if (const auto prefix = likePrefixLiteral(like->pattern)) {
        if (!indexes.hasIndex(like->column)) {
            return false;
        }
        path = AccessPath::PrefixLike;
        cost = std::max(static_cast<double>(rowCount) / 10.0, 1.0);
        return true;
    }

    if (const auto needle = likeContainsLiteral(like->pattern)) {
        if (needle->size() < 3) {
            return false;
        }
        IndexExpression trigram{IndexExpression::Kind::Trigram, like->column, {}};
        if (!indexes.hasExpressionIndex(trigram)) {
            return false;
        }
        const auto grams = extractTrigrams(*needle);
        if (grams.empty()) {
            return false;
        }
        IntersectPlan intersect;
        intersect.intersectProbes.reserve(grams.size());
        for (const auto &gram : grams) {
            IndexEqualityProbe probe;
            probe.column = like->column;
            probe.expression = trigram;
            probe.value = Value{gram};
            intersect.intersectProbes.push_back(std::move(probe));
        }
        path = AccessPath::Intersect;
        cost = std::max(static_cast<double>(rowCount) / 20.0, 1.0);
        trigramIntersect = std::move(intersect);
        return true;
    }
    return false;
}

} // namespace planner_detail
} // namespace VertexDB
