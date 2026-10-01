// Unit tests for Timeline and the memory budget helpers.
#include <gtest/gtest.h>

#include "vcam/buffer/memory_budget.hpp"
#include "vcam/buffer/timeline.hpp"

using vcam::Timeline;

TEST(Timeline, MakeCfrUsesExactTicks) {
    // 30 fps in the MP4-typical 1/15360 time base: 512 ticks per frame.
    Timeline timeline = Timeline::make_cfr(vcam::make_rational(30, 1), vcam::make_rational(1, 15360), 90);
    ASSERT_EQ(timeline.size(), 90u);
    EXPECT_EQ(timeline[1].pts_ticks, 512);
    EXPECT_EQ(timeline[89].pts_ticks, 89 * 512);
    EXPECT_EQ(timeline.end_ticks(), 90 * 512);
}

TEST(Timeline, IndexAtOrBefore) {
    Timeline timeline = Timeline::make_cfr(vcam::make_rational(10, 1), vcam::make_rational(1, 1000), 5);
    // frames at 0, 100, 200, 300, 400 ms
    EXPECT_EQ(timeline.index_at_or_before(0).value(), 0u);
    EXPECT_EQ(timeline.index_at_or_before(99).value(), 0u);
    EXPECT_EQ(timeline.index_at_or_before(100).value(), 1u);
    EXPECT_EQ(timeline.index_at_or_before(10'000).value(), 4u);
    EXPECT_FALSE(timeline.index_at_or_before(-1).has_value());
}

TEST(Timeline, EndUsesPreviousIntervalWhenDurationUnknown) {
    Timeline timeline(vcam::make_rational(1, 1000));
    timeline.append({0, 0, 0});
    timeline.append({1, 40, 0});
    EXPECT_EQ(timeline.end_ticks(), 80);
}

TEST(MemoryBudget, EstimateAndCheck) {
    auto format = vcam::make_frame_format(1280, 720, vcam::PixelFormat::YUYV).value();
    EXPECT_EQ(vcam::estimate_buffer_bytes(format, 1000), 1843200ull * 1000);
    EXPECT_TRUE(vcam::check_fits("clip", 100, 200, "").ok());
    vcam::Status status = vcam::check_fits("clip", 300, 200, "use --buffer-mode disk");
    EXPECT_EQ(status.code(), vcam::StatusCode::ResourceExhausted);
    EXPECT_NE(status.message().find("--buffer-mode disk"), std::string::npos);
}

TEST(MemoryBudget, SystemQueriesWork) {
    EXPECT_TRUE(vcam::available_memory_bytes().has_value());
    EXPECT_TRUE(vcam::free_disk_bytes("/tmp").has_value());
    EXPECT_GT(vcam::default_ram_budget(), 0u);
}
