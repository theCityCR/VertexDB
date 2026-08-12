#include "VertexDB/parser/recursive_cte.hpp"

#include "VertexDB/common/string_utils.hpp"

#include <algorithm>
#include <functional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace VertexDB {
namespace {

[[nodiscard]] std::size_t countTableRefsInSelect(const Select &select, std::string_view tableName) {
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
            count += countTableRefsInSelect(*arm.select, tableName);
        }
    }
    return count;
}

[[nodiscard]] std::string canonicalRecursiveName(const std::vector<CteEntry> &ctes,
                                                 std::string_view name) {
    for (const auto &cte : ctes) {
        if (cte.recursive && equalsIgnoreCase(cte.name, name)) {
            return cte.name;
        }
    }
    return {};
}

// Directed edges among recursive CTE indexes: arm of `from` references `to`.
[[nodiscard]] std::vector<std::vector<std::size_t>>
buildRecursiveRefGraph(const std::vector<CteEntry> &ctes,
                       const std::vector<std::size_t> &recursiveIndexes) {
    std::unordered_map<std::size_t, std::size_t> dense;
    for (std::size_t d = 0; d < recursiveIndexes.size(); ++d) {
        dense[recursiveIndexes[d]] = d;
    }
    std::vector<std::vector<std::size_t>> adj(recursiveIndexes.size());
    for (std::size_t d = 0; d < recursiveIndexes.size(); ++d) {
        const auto &cte = ctes[recursiveIndexes[d]];
        if (!cte.recursiveArm) {
            continue;
        }
        for (std::size_t other = 0; other < recursiveIndexes.size(); ++other) {
            if (countTableRefsInSelect(*cte.recursiveArm, ctes[recursiveIndexes[other]].name) > 0) {
                adj[d].push_back(other);
            }
        }
    }
    return adj;
}

// Kosaraju: returns sccId per dense recursive index; ids are dense 0..sccCount-1.
[[nodiscard]] std::pair<std::vector<std::size_t>, std::size_t>
stronglyConnectedComponents(const std::vector<std::vector<std::size_t>> &adj) {
    const std::size_t n = adj.size();
    std::vector<std::vector<std::size_t>> radj(n);
    for (std::size_t u = 0; u < n; ++u) {
        for (const auto v : adj[u]) {
            radj[v].push_back(u);
        }
    }
    std::vector<char> seen(n, 0);
    std::vector<std::size_t> order;
    order.reserve(n);
    const std::function<void(std::size_t)> dfs1 = [&](std::size_t u) {
        seen[u] = 1;
        for (const auto v : adj[u]) {
            if (!seen[v]) {
                dfs1(v);
            }
        }
        order.push_back(u);
    };
    for (std::size_t u = 0; u < n; ++u) {
        if (!seen[u]) {
            dfs1(u);
        }
    }
    std::vector<std::size_t> scc(n, 0);
    std::size_t sccCount = 0;
    std::fill(seen.begin(), seen.end(), 0);
    const std::function<void(std::size_t, std::size_t)> dfs2 = [&](std::size_t u, std::size_t id) {
        seen[u] = 1;
        scc[u] = id;
        for (const auto v : radj[u]) {
            if (!seen[v]) {
                dfs2(v, id);
            }
        }
    };
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        if (!seen[*it]) {
            dfs2(*it, sccCount++);
        }
    }
    return {std::move(scc), sccCount};
}

} // namespace

std::size_t countTableRefs(const Select &select, std::string_view tableName) {
    return countTableRefsInSelect(select, tableName);
}

bool selectReferencesTable(const Select &select, std::string_view tableName) {
    return countTableRefsInSelect(select, tableName) > 0;
}

std::vector<std::size_t> recursiveCteIndexes(const std::vector<CteEntry> &ctes) {
    std::vector<std::size_t> indexes;
    for (std::size_t i = 0; i < ctes.size(); ++i) {
        if (ctes[i].recursive) {
            indexes.push_back(i);
        }
    }
    return indexes;
}

std::vector<std::string> referencedRecursiveCteNames(const Select &select,
                                                     const std::vector<CteEntry> &ctes) {
    std::vector<std::string> names;
    std::unordered_set<std::string> seen;
    for (const auto &cte : ctes) {
        if (!cte.recursive) {
            continue;
        }
        if (countTableRefsInSelect(select, cte.name) == 0) {
            continue;
        }
        if (seen.insert(cte.name).second) {
            names.push_back(cte.name);
        }
    }
    return names;
}

std::vector<CteEntry> recursiveSccContaining(const std::vector<CteEntry> &ctes,
                                             std::string_view name) {
    const auto recursiveIndexes = recursiveCteIndexes(ctes);
    if (recursiveIndexes.empty()) {
        return {};
    }
    const auto canon = canonicalRecursiveName(ctes, name);
    if (canon.empty()) {
        return {};
    }
    const auto adj = buildRecursiveRefGraph(ctes, recursiveIndexes);
    const auto [scc, sccCount] = stronglyConnectedComponents(adj);
    (void)sccCount;
    std::size_t targetDense = recursiveIndexes.size();
    for (std::size_t d = 0; d < recursiveIndexes.size(); ++d) {
        if (equalsIgnoreCase(ctes[recursiveIndexes[d]].name, canon)) {
            targetDense = d;
            break;
        }
    }
    if (targetDense >= recursiveIndexes.size()) {
        return {};
    }
    const std::size_t targetScc = scc[targetDense];
    std::vector<CteEntry> group;
    for (std::size_t d = 0; d < recursiveIndexes.size(); ++d) {
        if (scc[d] == targetScc) {
            group.push_back(ctes[recursiveIndexes[d]]);
        }
    }
    return group;
}

std::vector<CteEntry> recursiveMaterializationOrder(const std::vector<CteEntry> &ctes) {
    const auto recursiveIndexes = recursiveCteIndexes(ctes);
    if (recursiveIndexes.empty()) {
        return {};
    }
    const auto adj = buildRecursiveRefGraph(ctes, recursiveIndexes);
    const auto [sccOf, sccCount] = stronglyConnectedComponents(adj);

    std::vector<std::vector<std::size_t>> members(sccCount);
    for (std::size_t d = 0; d < recursiveIndexes.size(); ++d) {
        members[sccOf[d]].push_back(d);
    }

    std::vector<std::unordered_set<std::size_t>> cond(sccCount);
    for (std::size_t u = 0; u < adj.size(); ++u) {
        for (const auto v : adj[u]) {
            if (sccOf[u] != sccOf[v]) {
                cond[sccOf[u]].insert(sccOf[v]);
            }
        }
    }

    // Kahn topo: edge a→b in adj means a's arm refs b, so b must be ready before a.
    // Condensation edge u→v means u depends on v; process dependencies first.
    std::vector<std::unordered_set<std::size_t>> producers(sccCount);
    std::vector<std::size_t> remaining(sccCount, 0);
    for (std::size_t u = 0; u < sccCount; ++u) {
        for (const auto v : cond[u]) {
            producers[v].insert(u);
            ++remaining[u];
        }
    }
    std::vector<std::size_t> queue;
    for (std::size_t id = 0; id < sccCount; ++id) {
        if (remaining[id] == 0) {
            queue.push_back(id);
        }
    }
    std::vector<std::size_t> topo;
    topo.reserve(sccCount);
    for (std::size_t qi = 0; qi < queue.size(); ++qi) {
        const auto id = queue[qi];
        topo.push_back(id);
        for (const auto consumer : producers[id]) {
            if (--remaining[consumer] == 0) {
                queue.push_back(consumer);
            }
        }
    }
    if (topo.size() != sccCount) {
        // Cycle among SCCs should be impossible; fall back to declaration order.
        topo.clear();
        for (std::size_t id = 0; id < sccCount; ++id) {
            topo.push_back(id);
        }
    }

    std::vector<CteEntry> ordered;
    ordered.reserve(recursiveIndexes.size());
    for (const auto sccId : topo) {
        for (const auto dense : members[sccId]) {
            ordered.push_back(ctes[recursiveIndexes[dense]]);
        }
    }
    return ordered;
}

void validateRecursiveCtes(const std::vector<CteEntry> &ctes) {
    const auto recursiveIndexes = recursiveCteIndexes(ctes);
    if (recursiveIndexes.empty()) {
        return;
    }

    for (const std::size_t i : recursiveIndexes) {
        const auto &cte = ctes[i];
        if (!cte.body || !cte.recursiveArm) {
            throw std::runtime_error("recursive CTE is missing anchor or recursive arm");
        }
        if (cte.recursiveBindAccumulator && !cte.recursive) {
            throw std::runtime_error("AS ACCUMULATOR requires a recursive CTE");
        }
        // Anchors must not reference any recursive CTE in this WITH list.
        for (const std::size_t j : recursiveIndexes) {
            if (countTableRefsInSelect(*cte.body, ctes[j].name) != 0) {
                throw std::runtime_error("recursive CTE anchor must not reference a recursive CTE");
            }
        }
        const auto refs = referencedRecursiveCteNames(*cte.recursiveArm, ctes);
        if (refs.size() != 1) {
            throw std::runtime_error(
                "recursive CTE arm must reference exactly one recursive CTE name");
        }
        if (countTableRefsInSelect(*cte.recursiveArm, refs[0]) != 1) {
            throw std::runtime_error(
                "recursive CTE arm must reference the recursive CTE name exactly once");
        }
    }

    // Condensation must be acyclic (mutual recursion lives inside an SCC; cross-SCC cycles reject).
    const auto adj = buildRecursiveRefGraph(ctes, recursiveIndexes);
    const auto [sccOf, sccCount] = stronglyConnectedComponents(adj);
    std::vector<std::unordered_set<std::size_t>> cond(sccCount);
    for (std::size_t u = 0; u < adj.size(); ++u) {
        for (const auto v : adj[u]) {
            if (sccOf[u] != sccOf[v]) {
                cond[sccOf[u]].insert(sccOf[v]);
            }
        }
    }
    std::vector<char> visiting(sccCount, 0);
    std::vector<char> visited(sccCount, 0);
    const std::function<bool(std::size_t)> cycle = [&](std::size_t u) -> bool {
        if (visiting[u]) {
            return true;
        }
        if (visited[u]) {
            return false;
        }
        visiting[u] = 1;
        for (const auto v : cond[u]) {
            if (cycle(v)) {
                return true;
            }
        }
        visiting[u] = 0;
        visited[u] = 1;
        return false;
    };
    for (std::size_t id = 0; id < sccCount; ++id) {
        if (cycle(id)) {
            throw std::runtime_error("recursive CTE dependency cycle is not supported");
        }
    }
}

} // namespace VertexDB
