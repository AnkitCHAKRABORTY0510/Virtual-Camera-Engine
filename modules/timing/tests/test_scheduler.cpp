// Unit tests for the absolute-deadline scheduler.
// Most tests use SimulatedClock, so they are exact and independent of machine load.
#include <gtest/gtest.h>

#include "vcam/core/clock_time.hpp"
#include "vcam/timing/clock.hpp"
#include "vcam/timing/realtime.hpp"
#include "vcam/timing/scheduler.hpp"

using vcam::Rational;
using vcam::Scheduler;
using vcam::SchedulerOptions;
using vcam::SimulatedClock;

namespace {
SchedulerOptions no_margin() {
    SchedulerOptions options;
    options.start_margin_ns = 0;
    return options;
}
}  // namespace

TEST(Scheduler, DeadlinesAreExactAndDoNotDrift) {
    SimulatedClock clock(1'000'000'000);
    Scheduler scheduler(clock, vcam::make_rational(30, 1), no_margin());
    scheduler.start();
    const int64_t t0 = scheduler.start_ns();

    for (uint64_t n = 0; n < 3000; ++n) {  // 100 s of output
        vcam::SlotTiming slot = scheduler.wait_for_next_slot();
        ASSERT_EQ(slot.index, n);
        ASSERT_EQ(slot.missed_before, 0u);
        ASSERT_EQ(slot.wake_ns, slot.target_ns);  // simulated: wakes exactly on time
    }
    // Slot 30 is exactly 1 s after T0, slot 3000 exactly 100 s: no accumulated error.
    EXPECT_EQ(scheduler.slot_time_ns(30) - t0, 1'000'000'000);
    EXPECT_EQ(scheduler.slot_time_ns(3000) - t0, 100'000'000'000);
    // Individual periods are 33 333 333 or 33 333 334 ns (exact 1/30 s rounded up).
    const int64_t period = scheduler.slot_time_ns(1) - scheduler.slot_time_ns(0);
    EXPECT_TRUE(period == 33'333'334 || period == 33'333'333);
}

TEST(Scheduler, NtscRateIsExactOverLongRuns) {
    SimulatedClock clock;
    Scheduler scheduler(clock, vcam::make_rational(30000, 1001), no_margin());
    scheduler.start();
    // 30000 frames at 29.97 fps last exactly 1001 seconds.
    EXPECT_EQ(scheduler.slot_time_ns(30000) - scheduler.start_ns(), 1001LL * 1'000'000'000);
}

TEST(Scheduler, SlotIndexInvertsSlotTime) {
    SimulatedClock clock;
    for (Rational fps : {vcam::make_rational(24, 1), vcam::make_rational(7, 3), vcam::make_rational(60000, 1001)}) {
        Scheduler scheduler(clock, fps, no_margin());
        scheduler.start();
        for (uint64_t n = 0; n < 5000; n += 7) {
            ASSERT_EQ(scheduler.slot_index_at(scheduler.slot_time_ns(n)), n) << vcam::to_string(fps);
            ASSERT_EQ(scheduler.slot_index_at(scheduler.slot_time_ns(n + 1) - 1), n);
        }
    }
}

TEST(Scheduler, LateWakeUpSkipsWholeSlotsInsteadOfBursting) {
    SimulatedClock clock;
    const int64_t period = 33'333'334;
    // The 11th sleep (number 10) wakes 3.5 periods late.
    clock.set_wake_delay([&](uint64_t sleep_number) { return sleep_number == 10 ? period * 7 / 2 : 0; });
    Scheduler scheduler(clock, vcam::make_rational(30, 1), no_margin());
    scheduler.start();

    for (uint64_t n = 0; n < 10; ++n) {
        EXPECT_EQ(scheduler.wait_for_next_slot().index, n);
    }
    vcam::SlotTiming late = scheduler.wait_for_next_slot();
    EXPECT_EQ(late.index, 13u);        // slots 10, 11, 12 were skipped
    EXPECT_EQ(late.missed_before, 3u);
    EXPECT_GE(late.wake_ns, late.target_ns);
    EXPECT_LT(late.wake_ns - late.target_ns, period);  // within slot 13's interval

    vcam::SlotTiming next = scheduler.wait_for_next_slot();
    EXPECT_EQ(next.index, 14u);        // back on the original grid
    EXPECT_EQ(next.missed_before, 0u);
    EXPECT_EQ(next.target_ns, scheduler.slot_time_ns(14));
}

TEST(Scheduler, SmallLatenessIsNotASkip) {
    SimulatedClock clock;
    clock.set_wake_delay([](uint64_t) { return 20'000'000; });  // 20 ms late every time (< 33 ms)
    Scheduler scheduler(clock, vcam::make_rational(30, 1), no_margin());
    scheduler.start();
    for (uint64_t n = 0; n < 100; ++n) {
        vcam::SlotTiming slot = scheduler.wait_for_next_slot();
        ASSERT_EQ(slot.index, n);
        ASSERT_EQ(slot.missed_before, 0u);
        ASSERT_EQ(slot.wake_ns - slot.target_ns, 20'000'000);  // lateness does not accumulate
    }
}

TEST(Scheduler, RebaseKeepsNumberingAndAvoidsFalseOverruns) {
    SimulatedClock clock;
    Scheduler scheduler(clock, vcam::make_rational(25, 1), no_margin());
    scheduler.start();
    for (int i = 0; i < 5; ++i) {
        scheduler.wait_for_next_slot();
    }
    clock.advance_ns(10'000'000'000);  // output was stopped for 10 s
    scheduler.rebase();
    vcam::SlotTiming slot = scheduler.wait_for_next_slot();
    EXPECT_EQ(slot.index, 5u);
    EXPECT_EQ(slot.missed_before, 0u);
}

TEST(Scheduler, RealClockKeepsTimeWithoutDrift) {
    // Real-time sanity check with generous limits (CI machines are noisy).
    vcam::configure_timing_thread({});
    vcam::MonotonicClock clock;
    Scheduler scheduler(clock, vcam::make_rational(100, 1));  // 10 ms slots
    scheduler.start();
    int64_t total_lateness = 0;
    uint64_t skipped = 0;
    for (int i = 0; i < 50; ++i) {
        vcam::SlotTiming slot = scheduler.wait_for_next_slot();
        ASSERT_GE(slot.wake_ns, slot.target_ns);  // never early
        total_lateness += slot.wake_ns - slot.target_ns;
        skipped += slot.missed_before;
    }
    EXPECT_LT(total_lateness / 50, 5'000'000);  // mean lateness well under 5 ms
    EXPECT_LE(skipped, 2u);
    // The 50 slots took 50 periods of wall-clock time (+ last lateness), not 50 * (period + overhead).
    const int64_t elapsed = clock.now_ns() - scheduler.start_ns();
    EXPECT_LT(elapsed, 49 * 10'000'000 + 20'000'000);
}

TEST(Scheduler, SpinModeIsNeverEarly) {
    vcam::MonotonicClock clock;
    SchedulerOptions options;
    options.spin_ns = 200'000;  // spin the last 0.2 ms
    Scheduler scheduler(clock, vcam::make_rational(200, 1), options);
    scheduler.start();
    for (int i = 0; i < 20; ++i) {
        vcam::SlotTiming slot = scheduler.wait_for_next_slot();
        ASSERT_GE(slot.wake_ns, slot.target_ns);
    }
}
