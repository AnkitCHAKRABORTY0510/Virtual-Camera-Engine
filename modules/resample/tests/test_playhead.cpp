// Unit tests for Playhead: FPS conversion cases A–E, EOF policies, pause, seek.
#include <gtest/gtest.h>

#include <vector>

#include "vcam/resample/playhead.hpp"

using vcam::EofPolicy;
using vcam::Playhead;
using vcam::SelectionPolicy;
using vcam::Timeline;

namespace {

const vcam::Rational kMp4TimeBase{1, 15360};

Timeline cfr(int64_t fps_num, int64_t fps_den, size_t frames, vcam::Rational time_base = kMp4TimeBase) {
    return Timeline::make_cfr(vcam::make_rational(fps_num, fps_den), time_base, frames);
}

// Source indices chosen for slots 0..count-1.
std::vector<uint64_t> indices(Playhead& playhead, uint64_t count) {
    std::vector<uint64_t> result;
    for (uint64_t n = 0; n < count; ++n) {
        result.push_back(playhead.select(n).source_index);
    }
    return result;
}

}  // namespace

// ---- Cases from docs/ARCHITECTURE.md §7.3 ------------------------------------------

TEST(Playhead, CaseA_SameRateMapsOneToOne) {
    Timeline timeline = cfr(30, 1, 900);
    Playhead playhead(timeline, vcam::make_rational(30, 1), SelectionPolicy::Hold, EofPolicy::Stop);
    playhead.start(0);
    for (uint64_t n = 0; n < 900; ++n) {
        vcam::Selection selection = playhead.select(n);
        ASSERT_EQ(selection.source_index, n);
        ASSERT_FALSE(selection.repeated);
        ASSERT_EQ(selection.skipped, 0u);
    }
    EXPECT_TRUE(playhead.select(900).end_of_stream);
}

TEST(Playhead, CaseA_SameRateIsExactInAwkwardTimeBases) {
    // 29.97 fps stored in a millisecond time base (33/34 ms gaps): still 1:1.
    Timeline timeline = cfr(30000, 1001, 3000, vcam::make_rational(1, 1000));
    Playhead playhead(timeline, vcam::make_rational(30000, 1001), SelectionPolicy::Hold, EofPolicy::Stop);
    playhead.start(0);
    for (uint64_t n = 0; n < 3000; ++n) {
        ASSERT_EQ(playhead.select(n).source_index, n);
    }
}

TEST(Playhead, CaseB_HalfRateDropsEveryOtherFrame) {
    Timeline timeline = cfr(30, 1, 90);
    Playhead playhead(timeline, vcam::make_rational(15, 1), SelectionPolicy::Hold, EofPolicy::Stop);
    playhead.start(0);
    EXPECT_EQ(indices(playhead, 6), (std::vector<uint64_t>{0, 2, 4, 6, 8, 10}));
    EXPECT_EQ(playhead.select(6).skipped, 1u);
}

TEST(Playhead, CaseC_DoubleRateHoldsEachFrameTwice) {
    Timeline timeline = cfr(15, 1, 45);
    Playhead playhead(timeline, vcam::make_rational(30, 1), SelectionPolicy::Hold, EofPolicy::Stop);
    playhead.start(0);
    EXPECT_EQ(indices(playhead, 8), (std::vector<uint64_t>{0, 0, 1, 1, 2, 2, 3, 3}));
    EXPECT_TRUE(playhead.select(8).source_index == 4);
    EXPECT_TRUE(playhead.select(9).repeated);
}

TEST(Playhead, CaseD_24To30RepeatsOneFrameInFive) {
    Timeline timeline = cfr(24, 1, 240);
    Playhead playhead(timeline, vcam::make_rational(30, 1), SelectionPolicy::Hold, EofPolicy::Stop);
    playhead.start(0);
    EXPECT_EQ(indices(playhead, 10), (std::vector<uint64_t>{0, 0, 1, 2, 3, 4, 4, 5, 6, 7}));
}

TEST(Playhead, NtscSourceAt30RepeatsOnceEvery1001Slots) {
    Timeline timeline = cfr(30000, 1001, 5000, vcam::make_rational(1, 30000));
    Playhead playhead(timeline, vcam::make_rational(30, 1), SelectionPolicy::Hold, EofPolicy::Stop);
    playhead.start(0);
    // Slot n shows frame floor(n * 1000 / 1001). Source frame 1 starts at
    // 33.367 ms, just AFTER slot 1 (33.333 ms), so the first repeat is slot 1,
    // then one every 1001 slots.
    std::vector<uint64_t> repeated_slots;
    for (uint64_t n = 0; n < 4000; ++n) {
        if (playhead.select(n).repeated) {
            repeated_slots.push_back(n);
        }
    }
    EXPECT_EQ(repeated_slots, (std::vector<uint64_t>{1, 1002, 2003, 3004}));
}

TEST(Playhead, CaseE_VariableFrameRateFollowsRealTimestamps) {
    // 3 frames 100 ms apart, then 3 frames 300 ms apart (ms time base).
    Timeline timeline(vcam::make_rational(1, 1000));
    const int64_t pts[] = {0, 100, 200, 300, 600, 900};
    for (uint64_t i = 0; i < 6; ++i) {
        timeline.append({i, pts[i], 0});
    }
    Playhead playhead(timeline, vcam::make_rational(10, 1), SelectionPolicy::Hold, EofPolicy::Stop);
    playhead.start(0);
    // Output every 100 ms.
    EXPECT_EQ(indices(playhead, 12), (std::vector<uint64_t>{0, 1, 2, 3, 3, 3, 4, 4, 4, 5, 5, 5}));
    EXPECT_TRUE(playhead.select(12).end_of_stream);  // last frame lasts 300 ms (previous gap)
}

TEST(Playhead, NearestPolicyRounds) {
    Timeline timeline = cfr(10, 1, 20, vcam::make_rational(1, 1000));  // frames every 100 ms
    Playhead hold(timeline, vcam::make_rational(15, 1), SelectionPolicy::Hold, EofPolicy::Stop);
    Playhead nearest(timeline, vcam::make_rational(15, 1), SelectionPolicy::Nearest, EofPolicy::Stop);
    hold.start(0);
    nearest.start(0);
    // Slot 1 is at 66.7 ms: HOLD shows frame 0, NEAREST frame 1 (100 ms is closer).
    EXPECT_EQ(hold.select(0).source_index, 0u);
    EXPECT_EQ(nearest.select(0).source_index, 0u);
    EXPECT_EQ(hold.select(1).source_index, 0u);
    EXPECT_EQ(nearest.select(1).source_index, 1u);
}

// ---- EOF policies ------------------------------------------------------------------------

TEST(Playhead, LoopWrapsWithUnchangedCadence) {
    Timeline timeline = cfr(30, 1, 10);
    Playhead playhead(timeline, vcam::make_rational(30, 1), SelectionPolicy::Hold, EofPolicy::Loop);
    playhead.start(0);
    std::vector<uint64_t> expected;
    for (int lap = 0; lap < 3; ++lap) {
        for (uint64_t i = 0; i < 10; ++i) {
            expected.push_back(i);
        }
    }
    EXPECT_EQ(indices(playhead, 30), expected);
    EXPECT_EQ(playhead.loops(), 2u);
}

TEST(Playhead, LoopKeepsFractionalPhase) {
    // 3 source frames at 10 fps (300 ms) shown at 4 fps (250 ms slots):
    // slot times 0, 250, 500(=200 after wrap), 750(=150), 1000(=100), ...
    Timeline timeline = cfr(10, 1, 3, vcam::make_rational(1, 1000));
    Playhead playhead(timeline, vcam::make_rational(4, 1), SelectionPolicy::Hold, EofPolicy::Loop);
    playhead.start(0);
    vcam::Selection s0 = playhead.select(0);
    vcam::Selection s1 = playhead.select(1);
    vcam::Selection s2 = playhead.select(2);
    vcam::Selection s3 = playhead.select(3);
    EXPECT_EQ(s0.source_index, 0u);
    EXPECT_EQ(s1.source_index, 2u);
    EXPECT_EQ(s2.source_index, 2u);  // 500 ms -> 200 ms after wrap
    EXPECT_TRUE(s2.looped);
    EXPECT_EQ(s3.source_index, 1u);  // 150 ms
}

TEST(Playhead, HoldKeepsShowingLastFrame) {
    Timeline timeline = cfr(30, 1, 5);
    Playhead playhead(timeline, vcam::make_rational(30, 1), SelectionPolicy::Hold, EofPolicy::Hold);
    playhead.start(0);
    indices(playhead, 5);
    for (uint64_t n = 5; n < 100; ++n) {
        vcam::Selection selection = playhead.select(n);
        ASSERT_EQ(selection.source_index, 4u);
        ASSERT_TRUE(selection.at_end);
        ASSERT_TRUE(selection.repeated);
    }
}

// ---- Pause, resume, seek ------------------------------------------------------------------

TEST(Playhead, PauseFreezesAndResumeContinuesWithoutJump) {
    Timeline timeline = cfr(30, 1, 300);
    Playhead playhead(timeline, vcam::make_rational(30, 1), SelectionPolicy::Hold, EofPolicy::Stop);
    playhead.start(0);
    indices(playhead, 10);              // slots 0..9 show frames 0..9
    playhead.pause(10);
    for (uint64_t n = 10; n < 50; ++n) {  // 40 paused slots
        ASSERT_EQ(playhead.select(n).source_index, 10u);
    }
    playhead.resume(50);
    EXPECT_EQ(playhead.select(50).source_index, 10u);  // continues exactly where it paused
    EXPECT_EQ(playhead.select(51).source_index, 11u);
    EXPECT_FALSE(playhead.select(52).repeated);
}

TEST(Playhead, SeekJumpsAndClamps) {
    Timeline timeline = cfr(30, 1, 300);  // 10 s
    Playhead playhead(timeline, vcam::make_rational(30, 1), SelectionPolicy::Hold, EofPolicy::Stop);
    playhead.start(0);
    indices(playhead, 100);
    ASSERT_TRUE(playhead.seek(100, 2.0).ok());
    EXPECT_EQ(playhead.select(100).source_index, 60u);
    EXPECT_EQ(playhead.select(101).source_index, 61u);
    EXPECT_NEAR(playhead.position_seconds(102), 2.0 + 2.0 / 30.0, 1e-9);

    ASSERT_TRUE(playhead.seek(102, 999.0).ok());  // beyond the end -> last frame
    EXPECT_EQ(playhead.select(102).source_index, 299u);
    ASSERT_TRUE(playhead.seek(103, -5.0).ok());   // before the start -> frame 0
    EXPECT_EQ(playhead.select(103).source_index, 0u);
}

TEST(Playhead, SeekWhilePausedShowsNewPositionAndStaysPaused) {
    Timeline timeline = cfr(30, 1, 300);
    Playhead playhead(timeline, vcam::make_rational(30, 1), SelectionPolicy::Hold, EofPolicy::Stop);
    playhead.start(0);
    playhead.select(0);
    playhead.pause(1);
    ASSERT_TRUE(playhead.seek(1, 5.0).ok());
    EXPECT_EQ(playhead.select(1).source_index, 150u);
    EXPECT_EQ(playhead.select(20).source_index, 150u);
    EXPECT_TRUE(playhead.paused());
}

TEST(Playhead, DurationAndParsing) {
    Timeline timeline = cfr(25, 1, 250);
    Playhead playhead(timeline, vcam::make_rational(30, 1), SelectionPolicy::Hold, EofPolicy::Loop);
    EXPECT_NEAR(playhead.duration_seconds(), 10.0, 1e-9);
    EXPECT_EQ(vcam::parse_eof_policy("loop").value(), EofPolicy::Loop);
    EXPECT_FALSE(vcam::parse_eof_policy("rewind").ok());
    EXPECT_EQ(vcam::parse_selection_policy("nearest").value(), SelectionPolicy::Nearest);
}
