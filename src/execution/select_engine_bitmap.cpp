#include "select_engine_scan_detail.hpp"

#include <algorithm>
#include <iterator>
#include <optional>
#include <utility>
#include <vector>

namespace VertexDB {
namespace select_scan_detail {
namespace {

std::vector<RowId> evalBitmapNode(const IndexBitmapNode &node, const Table &table);

// Combine children under Intersect or Union without allocating a wrapper node.
std::vector<RowId> evalBitmapChildren(IndexBitmapNode::Kind op,
                                      const std::vector<IndexBitmapNode> &children,
                                      const Table &table) {
    if (children.empty()) {
        return {};
    }

    std::optional<std::vector<RowId>> combined;
    for (const auto &child : children) {
        auto childIds = evalBitmapNode(child, table);
        if (op == IndexBitmapNode::Kind::Intersect) {
            if (childIds.empty()) {
                return {};
            }
            if (!combined) {
                combined = std::move(childIds);
                continue;
            }
            std::vector<RowId> next;
            next.reserve(std::min(combined->size(), childIds.size()));
            std::set_intersection(combined->begin(), combined->end(), childIds.begin(),
                                  childIds.end(), std::back_inserter(next));
            combined = std::move(next);
            if (combined->empty()) {
                return {};
            }
        } else {
            // Union
            if (childIds.empty()) {
                continue;
            }
            if (!combined) {
                combined = std::move(childIds);
                continue;
            }
            std::vector<RowId> next;
            next.reserve(combined->size() + childIds.size());
            std::set_union(combined->begin(), combined->end(), childIds.begin(), childIds.end(),
                           std::back_inserter(next));
            combined = std::move(next);
        }
    }
    return combined ? std::move(*combined) : std::vector<RowId>{};
}

std::vector<RowId> evalBitmapNode(const IndexBitmapNode &node, const Table &table) {
    if (node.kind == IndexBitmapNode::Kind::Probe) {
        auto rowIds = node.probe.expression
                          ? table.indexedLookup(*node.probe.expression, node.probe.value)
                          : table.indexedLookup(node.probe.column, node.probe.value);
        if (!rowIds) {
            return {};
        }
        std::sort(rowIds->begin(), rowIds->end());
        return std::move(*rowIds);
    }

    return evalBitmapChildren(node.kind, node.children, table);
}

} // namespace

std::vector<RowId> evalIntersectPlan(const IntersectPlan &path, const Table &table) {
    return evalBitmapChildren(IndexBitmapNode::Kind::Intersect, path.children, table);
}

std::vector<RowId> evalUnionPlan(const UnionPlan &path, const Table &table) {
    return evalBitmapChildren(IndexBitmapNode::Kind::Union, path.children, table);
}

} // namespace select_scan_detail
} // namespace VertexDB
