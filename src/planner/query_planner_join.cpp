#include "VertexDB/planner/query_planner.hpp"

#include "planner_detail.hpp"

#include "VertexDB/storage/table.hpp"

#include <algorithm>
#include <string>
#include <string_view>

namespace VertexDB {

namespace {

[[nodiscard]] bool isEquiJoin(const JoinClause &join) {
    return join.kind != JoinKind::Cross && join.op == ComparisonOperator::Equal;
}

[[nodiscard]] bool forcesNestedLoop(JoinKind kind) {
    return kind == JoinKind::LeftOuter || kind == JoinKind::RightOuter ||
           kind == JoinKind::FullOuter || kind == JoinKind::Cross;
}

[[nodiscard]] std::string_view joinKindLabel(JoinKind kind) {
    switch (kind) {
    case JoinKind::LeftOuter:
        return "left outer";
    case JoinKind::RightOuter:
        return "right outer";
    case JoinKind::FullOuter:
        return "full outer";
    case JoinKind::Cross:
        return "cross";
    case JoinKind::Inner:
        return "inner";
    }
    return "inner";
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

    if (join.kind == JoinKind::Cross) {
        plan.algorithm = JoinAlgorithm::NestedLoopIndexProbe;
        plan.estimatedCost = static_cast<double>(std::max<std::size_t>(leftRows, 1) *
                                                 std::max<std::size_t>(rightRows, 1));
        plan.probeTable.clear();
        plan.probeColumn.clear();
        plan.explanation = "cross nested loop join";
        return plan;
    }

    // Non-equi and outer joins cannot use hash join; fall back to nested-loop compare.
    if (!isEquiJoin(join) || forcesNestedLoop(join.kind)) {
        plan.algorithm = JoinAlgorithm::NestedLoopIndexProbe;
        plan.estimatedCost =
            static_cast<double>(std::max<std::size_t>(leftRows, 1) *
                                std::max<std::size_t>(rightRows, 1));
        if (isEquiJoin(join) && join.kind != JoinKind::RightOuter &&
            right.hasIndex(join.rightColumn)) {
            const double fanout =
                averageRowsPerKey(rightRows, distinctOrOne(right, right, join.rightColumn));
            plan.estimatedCost =
                static_cast<double>(std::max<std::size_t>(leftRows, 1)) * fanout;
            plan.probeTable = right.name();
            plan.probeColumn = join.rightColumn;
            plan.explanation = std::string{joinKindLabel(join.kind)} +
                               " nested loop join (index probe on " + right.name() + "." +
                               join.rightColumn + ")";
        } else if (isEquiJoin(join) && join.kind == JoinKind::RightOuter &&
                   left.hasIndex(join.leftColumn)) {
            // Preserve the right side: probe left indexes while scanning right.
            const double fanout =
                averageRowsPerKey(leftRows, distinctOrOne(left, left, join.leftColumn));
            plan.estimatedCost =
                static_cast<double>(std::max<std::size_t>(rightRows, 1)) * fanout;
            plan.outerIsLeft = false;
            plan.probeTable = left.name();
            plan.probeColumn = join.leftColumn;
            plan.explanation = std::string{joinKindLabel(join.kind)} +
                               " nested loop join (index probe on " + left.name() + "." +
                               join.leftColumn + ")";
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

    if (join.kind == JoinKind::Cross) {
        plan.algorithm = JoinAlgorithm::NestedLoopIndexProbe;
        plan.estimatedCost = static_cast<double>(std::max<std::size_t>(leftRows, 1) *
                                                 std::max<std::size_t>(rightRows, 1));
        plan.explanation = "cross nested loop join";
        return plan;
    }

    if (!isEquiJoin(join) || forcesNestedLoop(join.kind)) {
        plan.algorithm = JoinAlgorithm::NestedLoopIndexProbe;
        plan.estimatedCost =
            static_cast<double>(std::max<std::size_t>(leftRows, 1) *
                                std::max<std::size_t>(rightRows, 1));
        // Intermediate left rows have no indexes; RIGHT/FULL scan both sides. LEFT/equi may
        // still probe a right-side index.
        if (isEquiJoin(join) && join.kind != JoinKind::RightOuter &&
            right.hasIndex(join.rightColumn)) {
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
