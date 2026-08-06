#include "VertexDB/storage/histogram.hpp"

#include <algorithm>
#include <cmath>
#include <variant>

namespace VertexDB {
namespace {

[[nodiscard]] double clampFraction(double value) {
    if (value < 0.0) {
        return 0.0;
    }
    if (value > 1.0) {
        return 1.0;
    }
    return value;
}

[[nodiscard]] std::uint64_t distinctInRange(const std::vector<Value> &sorted, std::size_t begin,
                                            std::size_t end) {
    if (begin >= end) {
        return 0;
    }
    std::uint64_t distinct = 1;
    for (std::size_t i = begin + 1; i < end; ++i) {
        if (!(sorted[i] == sorted[i - 1])) {
            ++distinct;
        }
    }
    return distinct;
}

// Approximate how many rows in a bucket satisfy `column > value` assuming uniformity.
[[nodiscard]] bool valueGreater(const Value &lhs, const Value &rhs) {
    return rhs < lhs;
}

[[nodiscard]] std::uint64_t rowsGreaterInBucket(const HistogramBucket &bucket, const Value &value) {
    if (!valueGreater(bucket.upper, value)) {
        return 0;
    }
    if (valueGreater(bucket.lower, value)) {
        return bucket.rowCount;
    }
    // value is inside [lower, upper].
    if (bucket.distinctCount <= 1) {
        return valueGreater(bucket.upper, value) ? bucket.rowCount : 0;
    }
    if (!value.isNull() && !bucket.lower.isNull() && value.type() == bucket.lower.type() &&
        (value.type() == ColumnType::Int || value.type() == ColumnType::Double)) {
        const double lo = value.type() == ColumnType::Int
                              ? static_cast<double>(std::get<std::int64_t>(bucket.lower.data()))
                              : std::get<double>(bucket.lower.data());
        const double hi = value.type() == ColumnType::Int
                              ? static_cast<double>(std::get<std::int64_t>(bucket.upper.data()))
                              : std::get<double>(bucket.upper.data());
        const double v = value.type() == ColumnType::Int
                             ? static_cast<double>(std::get<std::int64_t>(value.data()))
                             : std::get<double>(value.data());
        if (hi <= lo) {
            return valueGreater(bucket.upper, value) ? bucket.rowCount : 0;
        }
        const double fraction = (hi - v) / (hi - lo);
        return static_cast<std::uint64_t>(
            std::llround(clampFraction(fraction) * static_cast<double>(bucket.rowCount)));
    }
    // Strings / mixed: split the bucket by distinct keys roughly in half above the bound.
    return std::max<std::uint64_t>(bucket.rowCount / 2, 1);
}

[[nodiscard]] std::uint64_t rowsLessInBucket(const HistogramBucket &bucket, const Value &value) {
    if (!(bucket.lower < value)) {
        return 0;
    }
    if (bucket.upper < value) {
        return bucket.rowCount;
    }
    if (bucket.distinctCount <= 1) {
        return bucket.lower < value ? bucket.rowCount : 0;
    }
    if (!value.isNull() && !bucket.lower.isNull() && value.type() == bucket.lower.type() &&
        (value.type() == ColumnType::Int || value.type() == ColumnType::Double)) {
        const double lo = value.type() == ColumnType::Int
                              ? static_cast<double>(std::get<std::int64_t>(bucket.lower.data()))
                              : std::get<double>(bucket.lower.data());
        const double hi = value.type() == ColumnType::Int
                              ? static_cast<double>(std::get<std::int64_t>(bucket.upper.data()))
                              : std::get<double>(bucket.upper.data());
        const double v = value.type() == ColumnType::Int
                             ? static_cast<double>(std::get<std::int64_t>(value.data()))
                             : std::get<double>(value.data());
        if (hi <= lo) {
            return bucket.lower < value ? bucket.rowCount : 0;
        }
        const double fraction = (v - lo) / (hi - lo);
        return static_cast<std::uint64_t>(
            std::llround(clampFraction(fraction) * static_cast<double>(bucket.rowCount)));
    }
    return std::max<std::uint64_t>(bucket.rowCount / 2, 1);
}

} // namespace

ColumnHistogram buildEquiHeightHistogram(std::string column, std::vector<Value> sortedValues,
                                         std::size_t maxBuckets) {
    ColumnHistogram histogram;
    histogram.column = std::move(column);
    histogram.rowCount = static_cast<std::uint64_t>(sortedValues.size());
    if (sortedValues.empty()) {
        return histogram;
    }

    histogram.distinctCount = distinctInRange(sortedValues, 0, sortedValues.size());
    const std::size_t bucketCount =
        std::max<std::size_t>(1, std::min(maxBuckets, sortedValues.size()));
    histogram.buckets.reserve(bucketCount);

    for (std::size_t bucketIndex = 0; bucketIndex < bucketCount; ++bucketIndex) {
        const std::size_t begin = (bucketIndex * sortedValues.size()) / bucketCount;
        const std::size_t end = ((bucketIndex + 1) * sortedValues.size()) / bucketCount;
        HistogramBucket bucket;
        bucket.lower = sortedValues[begin];
        bucket.upper = sortedValues[end - 1];
        bucket.rowCount = static_cast<std::uint64_t>(end - begin);
        bucket.distinctCount = distinctInRange(sortedValues, begin, end);
        histogram.buckets.push_back(std::move(bucket));
    }
    return histogram;
}

double histogramRangeSelectivity(const ColumnHistogram &histogram, ComparisonOperator op,
                                 const Value &value) {
    if (histogram.rowCount == 0 || histogram.buckets.empty()) {
        return 0.0;
    }
    std::uint64_t matching = 0;
    for (const auto &bucket : histogram.buckets) {
        if (op == ComparisonOperator::Greater) {
            matching += rowsGreaterInBucket(bucket, value);
        } else if (op == ComparisonOperator::Less) {
            matching += rowsLessInBucket(bucket, value);
        }
    }
    matching = std::min(matching, histogram.rowCount);
    return clampFraction(static_cast<double>(matching) / static_cast<double>(histogram.rowCount));
}

double histogramEqualitySelectivity(const ColumnHistogram &histogram) {
    if (histogram.rowCount == 0) {
        return 0.0;
    }
    const auto distinct = std::max<std::uint64_t>(histogram.distinctCount, 1);
    return clampFraction(1.0 / static_cast<double>(distinct));
}

double histogramInSelectivity(const ColumnHistogram &histogram, const std::vector<Value> &values) {
    if (histogram.rowCount == 0 || values.empty()) {
        return 0.0;
    }
    const auto distinct = std::max<std::uint64_t>(histogram.distinctCount, 1);
    const double perValue = 1.0 / static_cast<double>(distinct);
    return clampFraction(static_cast<double>(values.size()) * perValue);
}

} // namespace VertexDB
