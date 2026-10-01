// =============================================================================
// timestamp_stats.hpp — classify a stream as constant or variable frame rate
//
// Fed with every frame's timestamp in order, it keeps O(1) running statistics
// of the gaps between consecutive frames. After the whole file (or a long
// enough prefix), is_constant_frame_rate() tells whether all gaps are equal
// (within a tolerance of a few ticks for containers that round timestamps).
// =============================================================================
#pragma once

#include <cstdint>
#include <optional>

#include "vcam/core/rational.hpp"

namespace vcam {

class TimestampStats {
public:
    // Adds the next frame's presentation timestamp (in time-base ticks).
    void add(int64_t pts_ticks);

    uint64_t count() const { return count_; }
    int64_t first_pts() const { return first_pts_; }
    int64_t last_pts() const { return last_pts_; }
    int64_t min_interval() const { return min_interval_; }
    int64_t max_interval() const { return max_interval_; }

    // Mean gap between frames in ticks; nullopt with fewer than 2 frames.
    std::optional<double> mean_interval() const;

    // True when every gap is within `tolerance_ticks` of the smallest gap.
    // Needs at least 2 frames.
    bool is_constant_frame_rate(int64_t tolerance_ticks) const;

    // Frame rate implied by the mean gap: (count-1) / (last - first) seconds.
    std::optional<double> measured_fps(Rational time_base) const;

private:
    uint64_t count_ = 0;
    int64_t first_pts_ = 0;
    int64_t last_pts_ = 0;
    int64_t min_interval_ = 0;
    int64_t max_interval_ = 0;
};

}  // namespace vcam
