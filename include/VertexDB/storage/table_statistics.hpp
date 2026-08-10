#pragma once

// Per-table distinct counts and histograms. Implementation: src/storage/table_statistics.cpp.

#include "VertexDB/storage/histogram.hpp"
#include "VertexDB/storage/row.hpp"
#include "VertexDB/storage/row_store.hpp"

#include <cstddef>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace VertexDB {

// Lock-free statistics component. Its owning Table provides synchronization.
class TableStatistics {
  public:
    void analyze(std::span<const Column> schema, const RowStore &rowStore,
                 std::size_t maxBuckets = kDefaultHistogramBuckets);
    [[nodiscard]] std::optional<ColumnHistogram> columnHistogram(std::string_view column) const;
    [[nodiscard]] std::vector<ColumnHistogram> columnHistograms() const;
    void replaceColumnHistograms(std::vector<ColumnHistogram> histograms);
    void clearColumnHistograms();

  private:
    std::map<std::string, ColumnHistogram> histograms_;
};

} // namespace VertexDB
