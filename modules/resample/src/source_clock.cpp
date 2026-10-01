#include "vcam/resample/source_clock.hpp"

#include <cmath>

namespace vcam {

SourceClock::SourceClock(Rational time_base, Rational output_fps)
    : units_per_tick_(static_cast<Int128>(time_base.num) * output_fps.num),
      units_per_period_(static_cast<Int128>(output_fps.den) * time_base.den),
      units_per_second_(static_cast<Int128>(time_base.den) * output_fps.num) {}

void SourceClock::start(uint64_t slot, Int128 position) {
    anchor_slot_ = slot;
    anchor_position_ = position;
    paused_ = false;
}

Int128 SourceClock::position_at(uint64_t slot) const {
    if (paused_) {
        return paused_position_;
    }
    return anchor_position_ + static_cast<Int128>(slot - anchor_slot_) * units_per_period_;
}

int64_t SourceClock::ticks_at(uint64_t slot) const {
    const Int128 position = position_at(slot);
    // Floor division (positions can be negative only if set so explicitly).
    Int128 ticks = position / units_per_tick_;
    if (position % units_per_tick_ != 0 && position < 0) {
        ticks -= 1;
    }
    return static_cast<int64_t>(ticks);
}

double SourceClock::seconds_at(uint64_t slot) const {
    return static_cast<double>(position_at(slot)) / static_cast<double>(units_per_second_);
}

void SourceClock::pause(uint64_t slot) {
    if (!paused_) {
        paused_position_ = position_at(slot);
        paused_ = true;
    }
}

void SourceClock::resume(uint64_t slot) {
    if (paused_) {
        anchor_slot_ = slot;
        anchor_position_ = paused_position_;
        paused_ = false;
    }
}

void SourceClock::set_position(uint64_t slot, Int128 position) {
    anchor_slot_ = slot;
    anchor_position_ = position;
    if (paused_) {
        paused_position_ = position;
    }
}

Int128 SourceClock::seconds_to_units(double seconds) const {
    return static_cast<Int128>(std::llround(seconds * static_cast<double>(units_per_second_)));
}

}  // namespace vcam
