#include "vcam/metrics/histogram.hpp"

#include <algorithm>
#include <cmath>

namespace vcam {

namespace {

constexpr int64_t kMicro = 1'000;
constexpr int64_t kMilli = 1'000'000;

constexpr size_t kFineBuckets = 1000;    // [0, 1 ms)      in 1 µs steps
constexpr size_t kMediumBuckets = 990;   // [1, 100 ms)    in 100 µs steps
constexpr size_t kCoarseBuckets = 990;   // [100 ms, 10 s) in 10 ms steps
constexpr size_t kTotalBuckets = kFineBuckets + kMediumBuckets + kCoarseBuckets + 1;  // + overflow

}  // namespace

LatencyHistogram::LatencyHistogram() : buckets_(kTotalBuckets, 0) {}

size_t LatencyHistogram::bucket_for(int64_t value) {
    if (value < 1 * kMilli) {
        return static_cast<size_t>(value / kMicro);
    }
    if (value < 100 * kMilli) {
        return kFineBuckets + static_cast<size_t>((value - kMilli) / (100 * kMicro));
    }
    if (value < 10'000 * kMilli) {
        return kFineBuckets + kMediumBuckets + static_cast<size_t>((value - 100 * kMilli) / (10 * kMilli));
    }
    return kTotalBuckets - 1;
}

int64_t LatencyHistogram::bucket_upper_bound(size_t bucket) {
    if (bucket < kFineBuckets) {
        return static_cast<int64_t>(bucket + 1) * kMicro;
    }
    bucket -= kFineBuckets;
    if (bucket < kMediumBuckets) {
        return kMilli + static_cast<int64_t>(bucket + 1) * 100 * kMicro;
    }
    bucket -= kMediumBuckets;
    if (bucket < kCoarseBuckets) {
        return 100 * kMilli + static_cast<int64_t>(bucket + 1) * 10 * kMilli;
    }
    return 10'000 * kMilli;
}

void LatencyHistogram::add(int64_t value) {
    value = std::max<int64_t>(value, 0);
    ++buckets_[bucket_for(value)];
    if (count_ == 0) {
        min_ = max_ = value;
    } else {
        min_ = std::min(min_, value);
        max_ = std::max(max_, value);
    }
    ++count_;
    const double x = static_cast<double>(value);
    const double delta = x - mean_;
    mean_ += delta / static_cast<double>(count_);
    m2_ += delta * (x - mean_);
}

void LatencyHistogram::reset() {
    std::fill(buckets_.begin(), buckets_.end(), 0);
    count_ = 0;
    min_ = max_ = 0;
    mean_ = m2_ = 0.0;
}

double LatencyHistogram::mean_ns() const {
    return count_ ? mean_ : 0.0;
}

double LatencyHistogram::stddev_ns() const {
    return count_ > 1 ? std::sqrt(m2_ / static_cast<double>(count_ - 1)) : 0.0;
}

int64_t LatencyHistogram::percentile_ns(double percent) const {
    if (count_ == 0) {
        return 0;
    }
    // Rank of the sample we are looking for (1-based), e.g. P99 of 1000 = 990.
    const auto rank = static_cast<uint64_t>(std::ceil(percent / 100.0 * static_cast<double>(count_)));
    const uint64_t target = std::max<uint64_t>(rank, 1);
    uint64_t seen = 0;
    for (size_t bucket = 0; bucket < buckets_.size(); ++bucket) {
        seen += buckets_[bucket];
        if (seen >= target) {
            // Never report more than the true maximum.
            return std::min(bucket_upper_bound(bucket), max_);
        }
    }
    return max_;
}

}  // namespace vcam
