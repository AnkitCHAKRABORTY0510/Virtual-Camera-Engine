// Unit tests for SourceClock: exact slot -> source-time arithmetic, pause,
// resume and re-anchoring (seek). Used by stream mode and by Playhead.
#include <gtest/gtest.h>

#include "vcam/resample/source_clock.hpp"

using vcam::SourceClock;

namespace {
const vcam::Rational kMp4TimeBase{1, 15360};
}  // namespace

TEST(SourceClock, SameRateAdvancesOneFramePerSlot) {
    SourceClock clock(kMp4TimeBase, vcam::make_rational(30, 1));
    clock.start(100);                      // slot 100 shows source time 0
    EXPECT_EQ(clock.ticks_at(100), 0);
    EXPECT_EQ(clock.ticks_at(101), 512);   // 15360 / 30
    EXPECT_EQ(clock.ticks_at(130), 15360); // one second later
    EXPECT_DOUBLE_EQ(clock.seconds_at(130), 1.0);
}

TEST(SourceClock, NtscRateStaysExactOverAnHour) {
    // 30000/1001 fps over a 1/90000 time base: one period = 3003 ticks exactly.
    SourceClock clock(vcam::make_rational(1, 90000), vcam::make_rational(30000, 1001));
    clock.start(0);
    const uint64_t hour_of_slots = 107892;  // 3600 s × 29.97 fps
    EXPECT_EQ(clock.ticks_at(hour_of_slots), static_cast<int64_t>(hour_of_slots) * 3003);
}

TEST(SourceClock, PauseFreezesAndResumeContinuesWithoutAJump) {
    SourceClock clock(kMp4TimeBase, vcam::make_rational(30, 1));
    clock.start(0);
    clock.pause(10);
    EXPECT_TRUE(clock.paused());
    EXPECT_EQ(clock.ticks_at(10), 5120);
    EXPECT_EQ(clock.ticks_at(50), 5120);   // frozen
    clock.resume(50);
    EXPECT_EQ(clock.ticks_at(50), 5120);   // continues from where it stopped
    EXPECT_EQ(clock.ticks_at(51), 5632);
}

TEST(SourceClock, SetPositionReanchorsAndKeepsPauseState) {
    SourceClock clock(kMp4TimeBase, vcam::make_rational(30, 1));
    clock.start(0);
    clock.set_position(20, clock.seconds_to_units(7.0));
    EXPECT_EQ(clock.ticks_at(20), 7 * 15360);
    EXPECT_EQ(clock.ticks_at(21), 7 * 15360 + 512);

    clock.pause(30);
    clock.set_position(40, clock.ticks_to_units(1000));
    EXPECT_TRUE(clock.paused());           // a seek while paused stays paused
    EXPECT_EQ(clock.ticks_at(60), 1000);
    clock.resume(60);
    EXPECT_EQ(clock.ticks_at(61), 1512);
}

TEST(SourceClock, SlowerOutputAdvancesMoreThanOneFrame) {
    // 25 fps source time base (1/25), 10 fps output: 2.5 ticks per slot.
    SourceClock clock(vcam::make_rational(1, 25), vcam::make_rational(10, 1));
    clock.start(0);
    EXPECT_EQ(clock.ticks_at(1), 2);       // 2.5 rounded down
    EXPECT_EQ(clock.ticks_at(2), 5);
    EXPECT_EQ(clock.ticks_at(3), 7);
}
