// =============================================================================
// scheduler.hpp — absolute-deadline frame clock (the camera's heartbeat)
//
// Output slot n is due at
//
//     T_n = T0 + n * (1 / fps)            (computed exactly, never accumulated)
//
// wait_for_next_slot() sleeps until the next deadline and returns which slot
// it is. If the thread woke up so late that whole slots have already passed,
// those slots are SKIPPED (reported in `missed_before`) instead of being
// emitted in a burst — a real camera never delivers a backlog of old frames.
//
// The scheduler only decides WHEN. What is shown in a slot (resampler) and how
// it reaches Linux (virtual camera) are other modules' jobs.
// =============================================================================
#pragma once

#include <cstdint>

#include "vcam/core/rational.hpp"
#include "vcam/timing/clock.hpp"

namespace vcam {

struct SchedulerOptions {
    // Delay before slot 0, so the first frame is not already late.
    int64_t start_margin_ns = 5'000'000;  // 5 ms
    // Busy-wait the last `spin_ns` before each deadline for extra precision
    // (costs CPU). 0 = pure sleeping.
    int64_t spin_ns = 0;
};

struct SlotTiming {
    uint64_t index = 0;          // output slot number n
    int64_t target_ns = 0;       // T_n
    int64_t wake_ns = 0;         // when the thread actually woke up
    uint64_t missed_before = 0;  // slots skipped right before this one (overrun)
};

class Scheduler {
public:
    Scheduler(Clock& clock, Rational fps, SchedulerOptions options = {});

    // Sets T0 = now + start margin and the next slot to 0.
    void start();

    // Re-bases the clock so that the NEXT slot is due `start_margin` from now,
    // keeping the slot numbering. Used after the output was stopped for a while
    // (e.g. pause with output stopped) so resuming does not count huge overruns.
    void rebase();

    // Sleeps until the next slot's deadline and returns its timing.
    SlotTiming wait_for_next_slot();

    // T_n for any n (exact integer arithmetic, rounded up to whole ns).
    int64_t slot_time_ns(uint64_t index) const;

    // The slot whose interval [T_n, T_n+1) contains `time_ns` (0 if before T0).
    uint64_t slot_index_at(int64_t time_ns) const;

    Rational fps() const { return fps_; }
    int64_t start_ns() const { return t0_ns_; }
    uint64_t next_index() const { return next_index_; }
    Clock& clock() { return clock_; }

private:
    Clock& clock_;
    Rational fps_;
    Rational period_;  // 1 / fps seconds
    SchedulerOptions options_;
    int64_t t0_ns_ = 0;
    uint64_t index_offset_ = 0;  // slot number at t0 (non-zero after rebase)
    uint64_t next_index_ = 0;
};

}  // namespace vcam
