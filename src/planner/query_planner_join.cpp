#include "VertexDB/planner/query_planner.hpp"

#include "planner_detail.hpp"

#include "VertexDB/storage/table.hpp"

#include <algorithm>
#include <string>
#include <string_view>

namespace VertexDB {

namespace {

[[nodiscard]] bool isEquiJoin(const JoinClause &join) {
    return join.op == ComparisonOperator::Equal;
}

[[nodiscard]] std::string_view joinKindLabel(JoinKind kind) {
    return kind == JoinKind::LeftOuter ? "left outer" : "inner";
}

} // namespace

JoinPlan QueryPlanner::planJoin(const Table &left, const Table &right,
                                const JoinClause &join) const {
    using namespace planner_detail;

    JoinPlan plan;
    const auto leftRows = left.rowCount();
    const auto rightRows = right.rowCount();
    plan.estimatedRows = std::max(leftRows, rightRows);
    plan.outerIsLeft = true;

    // Non-equi and LEFT OUTER cannot use hash join; fall back to nested-loop compare.
    if (!isEquiJoin(join) || join.kind == JoinKind::LeftOuter) {
        plan.algorithm = JoinAlgorithm::NestedLoopIndexProbe;
        plan.estimatedCost =
            static_cast<double>(std::max<std::size_t>(leftRows, 1) *
                                std::max<std::size_t>(rightRows, 1));
        if (isEquiJoin(join) && right.hasIndex(join.rightColumn)) {
            const double fanout =
                averageRowsPerKey(rightRows, distinctOrOne(right, right, join.rightColumn));
            plan.estimatedCost =
                static_cast<double>(std::max<std::size_t>(leftRows, 1)) * fanout;
            plan.probeTable = right.name();
            plan.probeColumn = join.rightColumn;
            plan.explanation = std::string{joinKindLabel(join.kind)} +
                               " nested loop join (index probe on " + right.name() + "." +
                               join.rightColumn + ")";
        } else {
            plan.probeTable.clear();
            plan.probeColumn.clear();
            plan.explanation = std::string{joinKindLabel(join.kind)} + " nested loop join";
        }
        return plan;
    }

    // Build hash table on the right, probe from the left (matches the executor's hash join).
    plan.estimatedCost =
        static_cast<double>(std::max<std::size_t>(leftRows, 1) + std::max<std::size_t>(rightRows, 1));
    plan.explanation = "hash join";
    plan.algorithm = JoinAlgorithm::HashJoin;

    const bool rightIndexed = right.hasIndex(join.rightColumn);
    const bool leftIndexed = left.hasIndex(join.leftColumn);

    auto considerNested = [&](bool scanLeft, double cost, std::string_view probeTable,
                              std::string_view probeColumn) {
        if (cost < plan.estimatedCost) {
            plan.algorithm = JoinAlgorithm::NestedLoopIndexProbe;
            plan.estimatedCost = cost;
            plan.outerIsLeft = scanLeft;
            plan.probeTable = std::string{probeTable};
            plan.probeColumn = std::string{probeColumn};
            plan.explanation = std::string{"nested loop join (index probe on "} +
                               std::string{probeTable} + "." + std::string{probeColumn} + ")";
        }
    };

    if (rightIndexed) {
        const double fanout =
            averageRowsPerKey(rightRows, distinctOrOne(right, right, join.rightColumn));
        considerNested(true, static_cast<double>(std::max<std::size_t>(leftRows, 1)) * fanout,
                       right.name(), join.rightColumn);
    }
    if (leftIndexed) {
        const double fanout =
            averageRowsPerKey(leftRows, distinctOrOne(left, left, join.leftColumn));
        considerNested(false, static_cast<double>(std::max<std::size_t>(rightRows, 1)) * fanout,
                       left.name(), join.leftColumn);
    }

    return plan;
}

JoinPlan QueryPlanner::planJoinAgainstRows(std::size_t leftRows, const Table &right,
                                           const JoinClause &join) const {
    using namespace planner_detail;

    JoinPlan plan;
    const auto rightRows = right.rowCount();
    plan.estimatedRows = std::max(leftRows, rightRows);
    plan.outerIsLeft = true;

    if (!isEquiJoin(join) || join.kind == JoinKind::LeftOuter) {
        plan.algorithm = JoinAlgorithm::NestedLoopIndexProbe;
        plan.estimatedCost =
            static_cast<double>(std::max<std::size_t>(leftRows, 1) *
                                std::max<std::size_t>(rightRows, 1));
        if (isEquiJoin(join) && right.hasIndex(join.rightColumn)) {
            const double fanout =
                averageRowsPerKey(rightRows, distinctOrOne(right, right, join.rightColumn));
            plan.estimatedCost =
                static_cast<double>(std::max<std::size_t>(leftRows, 1)) * fanout;
            plan.probeTable = right.name();
            plan.probeColumn = join.rightColumn;
            plan.explanation = std::string{joinKindLabel(join.kind)} +
                               " nested loop join (index probe on " + right.name() + "." +
                               join.rightColumn + ")";
        } else {
            plan.explanation = std::string{joinKindLabel(join.kind)} + " nested loop join";
        }
        return plan;
    }

    plan.estimatedCost =
        static_cast<double>(std::max<std::size_t>(leftRows, 1) + std::max<std::size_t>(rightRows, 1));
    plan.explanation = "hash join";
    plan.algorithm = JoinAlgorithm::HashJoin;

    if (right.hasIndex(join.rightColumn)) {
        const double fanout =
            averageRowsPerKey(rightRows, distinctOrOne(right, right, join.rightColumn));
        const double cost = static_cast<double>(std::max<std::size_t>(leftRows, 1)) * fanout;
        if (cost < plan.estimatedCost) {
            plan.algorithm = JoinAlgorithm::NestedLoopIndexProbe;
            plan.estimatedCost = cost;
            plan.outerIsLeft = true;
            plan.probeTable = right.name();
            plan.probeColumn = join.rightColumn;
            plan.explanation = std::string{"nested loop join (index probe on "} + right.name() +
                               "." + join.rightColumn + ")";
        }
    }

    return plan;
}

} // namespace VertexDB
