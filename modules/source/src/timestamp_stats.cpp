#include "vcam/source/timestamp_stats.hpp"

#include <algorithm>

namespace vcam {

void TimestampStats::add(int64_t pts_ticks) {
    if (count_ == 0) {
        first_pts_ = pts_ticks;
    } else {
        const int64_t interval = pts_ticks - last_pts_;
        if (count_ == 1) {
            min_interval_ = interval;
            max_interval_ = interval;
        } else {
            min_interval_ = std::min(min_interval_, interval);
            max_interval_ = std::max(max_interval_, interval);
        }
    }
    last_pts_ = pts_ticks;
    ++count_;
}

std::optional<double> TimestampStats::mean_interval() const {
    if (count_ < 2) {
        return std::nullopt;
    }
    return static_cast<double>(last_pts_ - first_pts_) / static_cast<double>(count_ - 1);
}

bool TimestampStats::is_constant_frame_rate(int64_t tolerance_ticks) const {
    if (count_ < 2) {
        return true;  // nothing to compare; treat as constant
    }
    return max_interval_ - min_interval_ <= tolerance_ticks;
}

std::optional<double> TimestampStats::measured_fps(Rational time_base) const {
    if (count_ < 2 || last_pts_ == first_pts_ || !is_positive(time_base)) {
        return std::nullopt;
    }
    const double span_seconds = static_cast<double>(last_pts_ - first_pts_) * to_double(time_base);
    return static_cast<double>(count_ - 1) / span_seconds;
}

}  // namespace vcam
