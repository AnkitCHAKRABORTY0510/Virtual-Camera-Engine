// =============================================================================
// source_clock.hpp — "which source time is shown in output slot n?"
//
// The arithmetic shared by every playback mode (docs/ARCHITECTURE.md §7.1):
//
//     position(n) = anchor_position + (n − anchor_slot) · (1 / output_fps)
//
// Pause freezes the position; resume, seek and loop just move the anchor, so
// the output cadence never jumps.
//
// Exactness: positions are integers in a unit u = 1 / (time_base.den · fps.num)
// seconds, in which one source tick AND one output period are whole numbers:
//     one tick   = time_base.num · fps.num  u
//     one period = fps.den · time_base.den  u
// so no floating-point rounding can ever creep in, even over days.
// =============================================================================
#pragma once

#include <cstdint>

#include "vcam/core/rational.hpp"

namespace vcam {

class SourceClock {
public:
    SourceClock(Rational time_base, Rational output_fps);

    // Slot `slot` shows `position` (default: source time 0). Clears pause.
    void start(uint64_t slot, Int128 position = 0);

    // Source position at `slot` (frozen while paused). Slots must not go backwards.
    Int128 position_at(uint64_t slot) const;
    // Same, as whole source ticks (rounded down).
    int64_t ticks_at(uint64_t slot) const;
    double seconds_at(uint64_t slot) const;

    void pause(uint64_t slot);
    void resume(uint64_t slot);
    bool paused() const { return paused_; }

    // Re-anchor: from `slot` on, show `position` (stays paused if paused).
    void set_position(uint64_t slot, Int128 position);

    Int128 units_per_tick() const { return units_per_tick_; }
    Int128 units_per_period() const { return units_per_period_; }
    Int128 units_per_second() const { return units_per_second_; }
    Int128 ticks_to_units(int64_t ticks) const { return static_cast<Int128>(ticks) * units_per_tick_; }
    Int128 seconds_to_units(double seconds) const;

private:
    Int128 units_per_tick_;
    Int128 units_per_period_;
    Int128 units_per_second_;

    uint64_t anchor_slot_ = 0;
    Int128 anchor_position_ = 0;
    bool paused_ = false;
    Int128 paused_position_ = 0;
};

}  // namespace vcam
