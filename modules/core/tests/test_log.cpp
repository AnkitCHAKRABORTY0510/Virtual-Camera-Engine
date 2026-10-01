// Unit tests for log level handling and the rate limiter.
#include <gtest/gtest.h>

#include "vcam/core/log.hpp"
#include "vcam/core/status.hpp"

TEST(Log, ParseLevels) {
    vcam::LogLevel level = vcam::LogLevel::Info;
    EXPECT_TRUE(vcam::log::parse_level("DEBUG", level));
    EXPECT_EQ(level, vcam::LogLevel::Debug);
    EXPECT_TRUE(vcam::log::parse_level("warning", level));
    EXPECT_EQ(level, vcam::LogLevel::Warn);
    EXPECT_FALSE(vcam::log::parse_level("loud", level));
}

TEST(Log, EnabledFollowsLevel) {
    vcam::log::set_level(vcam::LogLevel::Warn);
    EXPECT_TRUE(vcam::log::enabled(vcam::LogLevel::Error));
    EXPECT_TRUE(vcam::log::enabled(vcam::LogLevel::Warn));
    EXPECT_FALSE(vcam::log::enabled(vcam::LogLevel::Info));
    vcam::log::set_level(vcam::LogLevel::Info);
}

TEST(Log, MacroDoesNotEvaluateDisabledMessages) {
    vcam::log::set_level(vcam::LogLevel::Error);
    int evaluations = 0;
    auto count = [&]() { ++evaluations; return 1; };
    VCAM_DEBUG("test", "value " << count());
    EXPECT_EQ(evaluations, 0);
    vcam::log::set_level(vcam::LogLevel::Info);
}

TEST(RateLimiter, AllowsFirstThenSuppresses) {
    vcam::RateLimiter limiter(60LL * 1'000'000'000LL);  // once per minute
    EXPECT_TRUE(limiter.allow());
    EXPECT_FALSE(limiter.allow());
    EXPECT_FALSE(limiter.allow());
    EXPECT_EQ(limiter.take_suppressed(), 2u);
    EXPECT_EQ(limiter.take_suppressed(), 0u);
}

TEST(Status, ToString) {
    EXPECT_EQ(vcam::Status().to_string(), "OK");
    vcam::Status error(vcam::StatusCode::NotFound, "no such file");
    EXPECT_FALSE(error.ok());
    EXPECT_EQ(error.to_string(), "NOT_FOUND: no such file");
}
