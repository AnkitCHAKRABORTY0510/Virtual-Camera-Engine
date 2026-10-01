// =============================================================================
// histogram.hpp — fixed-memory histogram for latency percentiles
//
// Storing every sample of a 24-hour run (2.6 million frames at 30 fps) just to
// compute P95/P99 would waste memory. Instead samples are counted in buckets
// whose width grows with the value:
//
//   0 .. 1 ms      1 µs wide buckets   (1000 buckets)
//   1 .. 100 ms    100 µs wide          (990 buckets)
//   100 ms .. 10 s 10 ms wide           (990 buckets)
//   >= 10 s        one overflow bucket
//
// add() is O(1) and memory is constant (~24 KB). Percentiles are exact to the
// bucket width (±1 µs below 1 ms, where camera timing errors usually are).
// Mean, standard deviation, min and max are exact.
// =============================================================================
#pragma once

#include <cstdint>
#include <vector>

namespace vcam {

class LatencyHistogram {
public:
    LatencyHistogram();

    // Records a non-negative duration in nanoseconds (negative values are clamped to 0).
    void add(int64_t value_ns);
    void reset();

    uint64_t count() const { return count_; }
    int64_t min_ns() const { return count_ ? min_ : 0; }
    int64_t max_ns() const { return count_ ? max_ : 0; }
    double mean_ns() const;
    double stddev_ns() const;

    // Smallest bucket upper bound below which `percent` % of samples fall
    // (percent in 0..100). Returns 0 when empty.
    int64_t percentile_ns(double percent) const;

private:
    static size_t bucket_for(int64_t value_ns);
    static int64_t bucket_upper_bound(size_t bucket);

    std::vector<uint64_t> buckets_;
    uint64_t count_ = 0;
    int64_t min_ = 0;
    int64_t max_ = 0;
    // Welford's online algorithm: numerically stable mean and variance.
    double mean_ = 0.0;
    double m2_ = 0.0;
};

}  // namespace vcam
