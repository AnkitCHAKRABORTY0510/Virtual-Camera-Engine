// =============================================================================
// timeline.hpp — the timestamps of every buffered source frame (no pixels)
//
// The resampler decides "which frame belongs to output slot n" using only this
// list, so that logic is testable without any image data.
// Frames are appended in presentation order with strictly increasing pts.
// =============================================================================
#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "vcam/core/frame.hpp"
#include "vcam/core/rational.hpp"

namespace vcam {

class Timeline {
public:
    Timeline() = default;
    explicit Timeline(Rational time_base) : time_base_(time_base) {}

    void set_time_base(Rational time_base) { time_base_ = time_base; }
    Rational time_base() const { return time_base_; }

    void reserve(size_t frames) { frames_.reserve(frames); }
    void append(const FrameTiming& timing) { frames_.push_back(timing); }
    void clear() { frames_.clear(); }

    size_t size() const { return frames_.size(); }
    bool empty() const { return frames_.empty(); }
    const FrameTiming& operator[](size_t index) const { return frames_[index]; }
    const FrameTiming& back() const { return frames_.back(); }

    // End of the last frame's display interval (pts + duration), in ticks.
    // This is the stream length used for looping and EOF.
    int64_t end_ticks() const;

    // Index of the last frame whose pts <= ticks (binary search, O(log n)).
    // nullopt when ticks is before the first frame.
    std::optional<size_t> index_at_or_before(int64_t ticks) const;

    // Builds a constant-frame-rate timeline: frame i at i * (1/fps) seconds,
    // expressed in `time_base` ticks (used by tests and tools).
    static Timeline make_cfr(Rational fps, Rational time_base, size_t frame_count);

private:
    Rational time_base_{1, 1};
    std::vector<FrameTiming> frames_;
};

}  // namespace vcam
