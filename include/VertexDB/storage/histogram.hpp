#pragma once

// Equi-height column histograms for ANALYZE / planner selectivity.
// Implementation: src/storage/histogram.cpp.

#include "VertexDB/common/comparison_operator.hpp"
#include "VertexDB/common/value.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace VertexDB {

constexpr std::size_t kDefaultHistogramBuckets = 32;

struct HistogramBucket {
    Value lower;
    Value upper;
    std::uint64_t rowCount{};
    std::uint64_t distinctCount{};
};

struct ColumnHistogram {
    std::string column;
    std::uint64_t rowCount{};       // non-null values analyzed
    std::uint64_t distinctCount{};  // distinct non-null values
    std::vector<HistogramBucket> buckets;
};

// Build an equi-height histogram from sorted non-null column values (already sorted ascending).
[[nodiscard]] ColumnHistogram buildEquiHeightHistogram(std::string column,
                                                       std::vector<Value> sortedValues,
                                                       std::size_t maxBuckets = kDefaultHistogramBuckets);

// Fraction of analyzed rows expected to satisfy `column op value` (0..1).
[[nodiscard]] double histogramRangeSelectivity(const ColumnHistogram &histogram,
                                               ComparisonOperator op, const Value &value);

// Fraction of analyzed rows expected to match any of `values` under uniform-within-distinct.
[[nodiscard]] double histogramInSelectivity(const ColumnHistogram &histogram,
                                            const std::vector<Value> &values);

// Equality selectivity ≈ 1/ndistinct when histogram is present.
[[nodiscard]] double histogramEqualitySelectivity(const ColumnHistogram &histogram);

} // namespace VertexDB
