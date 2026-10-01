// Unit tests for TimestampStats (CFR/VFR classification), no media needed.
#include <gtest/gtest.h>

#include "vcam/source/timestamp_stats.hpp"

TEST(TimestampStats, ConstantRate) {
    vcam::TimestampStats stats;
    for (int64_t i = 0; i < 90; ++i) {
        stats.add(i * 512);  // 30 fps in a 1/15360 time base
    }
    EXPECT_EQ(stats.count(), 90u);
    EXPECT_EQ(stats.min_interval(), 512);
    EXPECT_EQ(stats.max_interval(), 512);
    EXPECT_TRUE(stats.is_constant_frame_rate(1));
    EXPECT_NEAR(stats.measured_fps(vcam::make_rational(1, 15360)).value(), 30.0, 1e-9);
}

TEST(TimestampStats, MillisecondRoundingIsStillConstant) {
    // 30 fps in a 1/1000 time base: gaps alternate 33/34 ms.
    vcam::TimestampStats stats;
    for (int64_t i = 0; i < 60; ++i) {
        stats.add((i * 1000 + 15) / 30);
    }
    EXPECT_TRUE(stats.is_constant_frame_rate(1));
    EXPECT_FALSE(stats.is_constant_frame_rate(0));
}

TEST(TimestampStats, VariableRate) {
    vcam::TimestampStats stats;
    int64_t pts = 0;
    for (int i = 0; i < 30; ++i) { stats.add(pts); pts += 33; }  // 30 fps
    for (int i = 0; i < 30; ++i) { stats.add(pts); pts += 67; }  // 15 fps
    EXPECT_FALSE(stats.is_constant_frame_rate(1));
    EXPECT_EQ(stats.min_interval(), 33);
    EXPECT_EQ(stats.max_interval(), 67);
}

TEST(TimestampStats, FewerThanTwoFrames) {
    vcam::TimestampStats stats;
    EXPECT_FALSE(stats.mean_interval().has_value());
    stats.add(0);
    EXPECT_FALSE(stats.measured_fps(vcam::make_rational(1, 1000)).has_value());
    EXPECT_TRUE(stats.is_constant_frame_rate(0));
}
