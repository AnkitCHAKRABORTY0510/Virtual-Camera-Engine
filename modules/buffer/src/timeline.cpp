#include "vcam/buffer/timeline.hpp"

#include <algorithm>

namespace vcam {

int64_t Timeline::end_ticks() const {
    if (frames_.empty()) {
        return 0;
    }
    const FrameTiming& last = frames_.back();
    int64_t duration = last.duration_ticks;
    if (duration <= 0 && frames_.size() >= 2) {
        // Unknown duration: assume the previous frame interval repeats.
        duration = last.pts_ticks - frames_[frames_.size() - 2].pts_ticks;
    }
    return last.pts_ticks + std::max<int64_t>(duration, 1);
}

std::optional<size_t> Timeline::index_at_or_before(int64_t ticks) const {
    // std::upper_bound finds the first frame with pts > ticks (binary search on
    // the sorted pts). The frame just before it is the one "on screen" at ticks.
    auto first_after = std::upper_bound(frames_.begin(), frames_.end(), ticks,
                                        [](int64_t value, const FrameTiming& frame) { return value < frame.pts_ticks; });
    if (first_after == frames_.begin()) {
        return std::nullopt;
    }
    return static_cast<size_t>(std::distance(frames_.begin(), first_after) - 1);
}

Timeline Timeline::make_cfr(Rational fps, Rational time_base, size_t frame_count) {
    Timeline timeline(time_base);
    timeline.reserve(frame_count);
    const Rational frame_period = invert(fps);
    for (size_t i = 0; i < frame_count; ++i) {
        FrameTiming timing;
        timing.source_index = i;
        // pts_i = i * period, converted from seconds to ticks (rounded down,
        // like a muxer that stores integer ticks).
        timing.pts_ticks = rescale(static_cast<int64_t>(i), frame_period, time_base, Rounding::Down);
        const int64_t next = rescale(static_cast<int64_t>(i + 1), frame_period, time_base, Rounding::Down);
        timing.duration_ticks = next - timing.pts_ticks;
        timeline.append(timing);
    }
    return timeline;
}

}  // namespace vcam
