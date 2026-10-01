#include "vcam/timing/scheduler.hpp"

#include "vcam/core/clock_time.hpp"

namespace vcam {

namespace {
const Rational kNanosecond{1, kNanosPerSecond};
}

Scheduler::Scheduler(Clock& clock, Rational fps, SchedulerOptions options)
    : clock_(clock), fps_(fps), period_(invert(fps)), options_(options) {}

void Scheduler::start() {
    t0_ns_ = clock_.now_ns() + options_.start_margin_ns;
    index_offset_ = 0;
    next_index_ = 0;
}

void Scheduler::rebase() {
    t0_ns_ = clock_.now_ns() + options_.start_margin_ns;
    index_offset_ = next_index_;
}

int64_t Scheduler::slot_time_ns(uint64_t index) const {
    // (index - offset) periods after T0. rescale() multiplies with a 128-bit
    // intermediate: no overflow and no accumulated rounding. Rounding UP
    // guarantees slot_index_at(slot_time_ns(n)) == n.
    const auto slots = static_cast<int64_t>(index - index_offset_);
    return t0_ns_ + rescale(slots, period_, kNanosecond, Rounding::Up);
}

uint64_t Scheduler::slot_index_at(int64_t time_ns) const {
    if (time_ns <= t0_ns_) {
        return index_offset_;
    }
    // floor((time - T0) / period)
    const int64_t slots = rescale(time_ns - t0_ns_, kNanosecond, period_, Rounding::Down);
    return index_offset_ + static_cast<uint64_t>(slots);
}

SlotTiming Scheduler::wait_for_next_slot() {
    SlotTiming slot;
    slot.index = next_index_;
    slot.target_ns = slot_time_ns(slot.index);

    if (options_.spin_ns > 0) {
        // Sleep until shortly before the deadline, then poll the clock.
        clock_.sleep_until_ns(slot.target_ns - options_.spin_ns);
        while (clock_.now_ns() < slot.target_ns) {
            // busy wait (intentionally empty)
        }
    } else {
        clock_.sleep_until_ns(slot.target_ns);
    }
    slot.wake_ns = clock_.now_ns();

    // Overrun: we woke up inside a LATER slot's interval. Skip to that slot
    // rather than emitting the old ones late.
    const uint64_t current = slot_index_at(slot.wake_ns);
    if (current > slot.index) {
        slot.missed_before = current - slot.index;
        slot.index = current;
        slot.target_ns = slot_time_ns(current);
    }
    next_index_ = slot.index + 1;
    return slot;
}

}  // namespace vcam
