#include "vcam/resample/playhead.hpp"

#include <algorithm>
#include <cmath>

namespace vcam {

const char* eof_policy_name(EofPolicy policy) {
    switch (policy) {
        case EofPolicy::Stop: return "stop";
        case EofPolicy::Loop: return "loop";
        case EofPolicy::Hold: return "hold";
    }
    return "?";
}

const char* selection_policy_name(SelectionPolicy policy) {
    return policy == SelectionPolicy::Nearest ? "nearest" : "hold";
}

Result<EofPolicy> parse_eof_policy(const std::string& text) {
    if (text == "stop") return EofPolicy::Stop;
    if (text == "loop") return EofPolicy::Loop;
    if (text == "hold") return EofPolicy::Hold;
    return Status(StatusCode::InvalidArgument, "unknown EOF policy '" + text + "' (stop, loop, hold)");
}

Result<SelectionPolicy> parse_selection_policy(const std::string& text) {
    if (text == "hold") return SelectionPolicy::Hold;
    if (text == "nearest") return SelectionPolicy::Nearest;
    return Status(StatusCode::InvalidArgument, "unknown selection policy '" + text + "' (hold, nearest)");
}

Playhead::Playhead(const Timeline& timeline, Rational output_fps, SelectionPolicy selection, EofPolicy eof)
    : timeline_(timeline),
      selection_policy_(selection),
      eof_policy_(eof),
      clock_(timeline.time_base(), output_fps),
      end_units_(clock_.ticks_to_units(timeline.end_ticks())) {}

void Playhead::start(uint64_t first_slot) {
    clock_.start(first_slot);
    cursor_ = 0;
    previous_.reset();
    loops_ = 0;
}

uint64_t Playhead::index_for_position(Int128 position) {
    // Source tick at this position, rounded down (integer division of
    // non-negative values is floor).
    const auto target_ticks = static_cast<int64_t>(position / clock_.units_per_tick());
    const size_t size = timeline_.size();

    // HOLD: last frame with pts <= target. Usually the answer is at or just
    // after the previous one, so walk forward from the cursor (O(1) amortised);
    // if time went backwards (loop, seek) use a binary search instead.
    if (cursor_ >= size || timeline_[cursor_].pts_ticks > target_ticks) {
        cursor_ = timeline_.index_at_or_before(target_ticks).value_or(0);
    }
    while (cursor_ + 1 < size && timeline_[cursor_ + 1].pts_ticks <= target_ticks) {
        ++cursor_;
    }

    size_t chosen = cursor_;
    if (selection_policy_ == SelectionPolicy::Nearest && cursor_ + 1 < size) {
        // Compare exact distances (in units u) to this frame and the next one.
        const Int128 here = clock_.ticks_to_units(timeline_[cursor_].pts_ticks);
        const Int128 next = clock_.ticks_to_units(timeline_[cursor_ + 1].pts_ticks);
        if (next - position < position - here) {
            chosen = cursor_ + 1;
        }
    }
    return chosen;
}

Selection Playhead::select(uint64_t slot) {
    Selection selection;
    if (timeline_.empty()) {
        selection.end_of_stream = true;
        return selection;
    }

    Int128 position = clock_.position_at(slot);

    if (position >= end_units_) {
        switch (eof_policy_) {
            case EofPolicy::Stop:
                selection.end_of_stream = true;
                return selection;
            case EofPolicy::Hold:
                selection.at_end = true;
                position = end_units_ - 1;  // inside the last frame's interval
                break;
            case EofPolicy::Loop: {
                // Wrap around, keeping the fractional phase so the seam is timed
                // like every other frame boundary.
                loops_ += static_cast<uint64_t>(position / end_units_);
                position %= end_units_;
                clock_.set_position(slot, position);
                selection.looped = true;
                break;
            }
        }
    }

    const uint64_t index = index_for_position(position);
    selection.source_index = index;
    if (previous_) {
        if (selection.looped) {
            // Frames after the previous one up to the end, plus those before `index`.
            const uint64_t last = timeline_.size() - 1;
            selection.skipped = static_cast<uint32_t>((last - *previous_) + index);
        } else if (index == *previous_) {
            selection.repeated = true;
        } else if (index > *previous_ + 1) {
            selection.skipped = static_cast<uint32_t>(index - *previous_ - 1);
        }
    }
    previous_ = index;
    return selection;
}

void Playhead::pause(uint64_t slot) {
    clock_.pause(slot);
}

void Playhead::resume(uint64_t slot) {
    clock_.resume(slot);
}

Status Playhead::seek(uint64_t slot, double seconds) {
    if (!std::isfinite(seconds)) {
        return Status(StatusCode::InvalidArgument, "seek position must be a number");
    }
    if (timeline_.empty()) {
        return Status(StatusCode::InvalidArgument, "cannot seek: no frames");
    }
    // Clamp to [0, start of the last frame].
    Int128 position = clock_.seconds_to_units(std::max(0.0, seconds));
    position = std::min(position, clock_.ticks_to_units(timeline_.back().pts_ticks));
    clock_.set_position(slot, position);
    cursor_ = timeline_.size();  // force a binary search next time
    return Status::ok_status();
}

double Playhead::position_seconds(uint64_t slot) const {
    Int128 position = clock_.position_at(slot);
    if (eof_policy_ != EofPolicy::Loop && position > end_units_) {
        position = end_units_;
    }
    return static_cast<double>(position) / static_cast<double>(clock_.units_per_second());
}

double Playhead::duration_seconds() const {
    return static_cast<double>(end_units_) / static_cast<double>(clock_.units_per_second());
}

}  // namespace vcam
