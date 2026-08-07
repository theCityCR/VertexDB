#include "VertexDB/storage/table_statistics.hpp"

#include <algorithm>
#include <utility>

namespace VertexDB {

void TableStatistics::analyze(std::span<const Column> schema, const RowStore &rowStore,
                              std::size_t maxBuckets) {
    histograms_.clear();
    const auto entries = rowStore.liveEntries();
    for (std::size_t columnIndex = 0; columnIndex < schema.size(); ++columnIndex) {
        std::vector<Value> values;
        values.reserve(entries.size());
        for (const auto &[rowId, row] : entries) {
            (void)rowId;
            if (columnIndex < row.size() && !row[columnIndex].isNull()) {
                values.push_back(row[columnIndex]);
            }
        }
        std::sort(values.begin(), values.end());
        auto histogram =
            buildEquiHeightHistogram(schema[columnIndex].name, std::move(values), maxBuckets);
        histograms_.emplace(histogram.column, std::move(histogram));
    }
}

std::optional<ColumnHistogram>
TableStatistics::columnHistogram(std::string_view column) const {
    auto it = histograms_.find(std::string{column});
    if (it == histograms_.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::vector<ColumnHistogram> TableStatistics::columnHistograms() const {
    std::vector<ColumnHistogram> out;
    out.reserve(histograms_.size());
    for (const auto &[_, histogram] : histograms_) {
        out.push_back(histogram);
    }
    return out;
}

void TableStatistics::replaceColumnHistograms(std::vector<ColumnHistogram> histograms) {
    histograms_.clear();
    for (auto &histogram : histograms) {
        histograms_.emplace(histogram.column, std::move(histogram));
    }
}

void TableStatistics::clearColumnHistograms() { histograms_.clear(); }

} // namespace VertexDB
