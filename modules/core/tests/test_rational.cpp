// Unit tests for vcam::Rational (exact fractions for frame rates and time bases).
#include <gtest/gtest.h>

#include "vcam/core/rational.hpp"

using vcam::Rational;
using vcam::Rounding;

TEST(Rational, MakeReducesAndNormalisesSign) {
    Rational r = vcam::make_rational(60, 2);
    EXPECT_EQ(r.num, 30);
    EXPECT_EQ(r.den, 1);

    Rational negative = vcam::make_rational(3, -6);
    EXPECT_EQ(negative.num, -1);
    EXPECT_EQ(negative.den, 2);

    EXPECT_FALSE(vcam::is_valid(vcam::make_rational(1, 0)));
}

TEST(Rational, CompareIsExact) {
    // 30000/1001 (29.97...) is slightly less than 30.
    EXPECT_EQ(vcam::compare(vcam::make_rational(30000, 1001), vcam::make_rational(30, 1)), -1);
    EXPECT_EQ(vcam::compare(vcam::make_rational(2, 4), vcam::make_rational(1, 2)), 0);
    EXPECT_TRUE(vcam::make_rational(15360, 512) == vcam::make_rational(30, 1));
}

TEST(Rational, RescaleTicksToNanoseconds) {
    const Rational mp4_time_base = vcam::make_rational(1, 15360);
    const Rational nanoseconds = vcam::make_rational(1, 1'000'000'000);

    // Frame 1 of a 30 FPS MP4: 512 ticks = 33 333 333.33 ns.
    EXPECT_EQ(vcam::rescale(512, mp4_time_base, nanoseconds, Rounding::Down), 33'333'333);
    EXPECT_EQ(vcam::rescale(512, mp4_time_base, nanoseconds, Rounding::Nearest), 33'333'333);
    EXPECT_EQ(vcam::rescale(512, mp4_time_base, nanoseconds, Rounding::Up), 33'333'334);
}

TEST(Rational, RescaleRoundingOfNegativeValues) {
    // rescale(value, 1, 3) computes value / 3 with the chosen rounding.
    const Rational one = vcam::make_rational(1, 1);
    const Rational third = vcam::make_rational(3, 1);
    EXPECT_EQ(vcam::rescale(-7, one, third, Rounding::Down), -3);     // floor(-2.33) = -3
    EXPECT_EQ(vcam::rescale(-7, one, third, Rounding::Up), -2);       // ceil(-2.33)  = -2
    EXPECT_EQ(vcam::rescale(-7, one, third, Rounding::Nearest), -2);  // round(-2.33) = -2
    EXPECT_EQ(vcam::rescale(-8, one, third, Rounding::Nearest), -3);  // round(-2.67) = -3
}

TEST(Rational, RescaleDoesNotOverflowForLongRuns) {
    // One year of 120 FPS output slots converted to nanoseconds:
    // n * 1e9 / 120 overflows 64-bit if done naively (n * 1e9 alone is ~3.8e18 * 1e9).
    const int64_t slots_per_year = 120LL * 3600 * 24 * 365;
    const Rational slot = vcam::make_rational(1, 120);
    const Rational nanoseconds = vcam::make_rational(1, 1'000'000'000);
    EXPECT_EQ(vcam::rescale(slots_per_year, slot, nanoseconds, Rounding::Down),
              3600LL * 24 * 365 * 1'000'000'000LL);
}

TEST(Rational, ParseIntegerFractionAndDecimal) {
    auto thirty = vcam::parse_rational("30");
    ASSERT_TRUE(thirty.ok());
    EXPECT_EQ(thirty.value(), vcam::make_rational(30, 1));

    auto ntsc = vcam::parse_rational(" 30000/1001 ");
    ASSERT_TRUE(ntsc.ok());
    EXPECT_EQ(ntsc.value(), vcam::make_rational(30000, 1001));

    auto twelve_and_half = vcam::parse_rational("12.5");
    ASSERT_TRUE(twelve_and_half.ok());
    EXPECT_EQ(twelve_and_half.value(), vcam::make_rational(25, 2));
}

TEST(Rational, ParseSnapsNtscDecimals) {
    EXPECT_EQ(vcam::parse_rational("29.97").value(), vcam::make_rational(30000, 1001));
    EXPECT_EQ(vcam::parse_rational("23.976").value(), vcam::make_rational(24000, 1001));
    EXPECT_EQ(vcam::parse_rational("59.94").value(), vcam::make_rational(60000, 1001));
    EXPECT_EQ(vcam::parse_rational("119.88").value(), vcam::make_rational(120000, 1001));
    // 30.0 must stay exactly 30, not be snapped.
    EXPECT_EQ(vcam::parse_rational("30.0").value(), vcam::make_rational(30, 1));
}

TEST(Rational, ParseRejectsJunk) {
    EXPECT_FALSE(vcam::parse_rational("").ok());
    EXPECT_FALSE(vcam::parse_rational("abc").ok());
    EXPECT_FALSE(vcam::parse_rational("30/0").ok());
    EXPECT_FALSE(vcam::parse_rational("30fps").ok());
    EXPECT_FALSE(vcam::parse_rational("1.").ok());
}

TEST(Rational, ToString) {
    EXPECT_EQ(vcam::to_string(vcam::make_rational(30, 1)), "30");
    EXPECT_EQ(vcam::to_string(vcam::make_rational(30000, 1001)), "30000/1001");
}
