// Unit tests for human-readable number formatting/parsing.
#include <gtest/gtest.h>

#include "vcam/core/text_format.hpp"

TEST(TextFormat, FormatBytes) {
    EXPECT_EQ(vcam::format_bytes(512), "512 B");
    EXPECT_EQ(vcam::format_bytes(1536), "1.5 KiB");
    EXPECT_EQ(vcam::format_bytes(1843200), "1.8 MiB");
    EXPECT_EQ(vcam::format_bytes(3ull << 30), "3.0 GiB");
}

TEST(TextFormat, FormatSeconds) {
    EXPECT_EQ(vcam::format_seconds(12.25), "0:12.250");
    EXPECT_EQ(vcam::format_seconds(3725.5), "1:02:05.500");
}

TEST(TextFormat, ParseBytes) {
    uint64_t value = 0;
    ASSERT_TRUE(vcam::parse_bytes("512M", value));
    EXPECT_EQ(value, 512ull << 20);
    ASSERT_TRUE(vcam::parse_bytes("1.5GiB", value));
    EXPECT_EQ(value, 3ull << 29);
    ASSERT_TRUE(vcam::parse_bytes("1000", value));
    EXPECT_EQ(value, 1000u);
    EXPECT_FALSE(vcam::parse_bytes("lots", value));
    EXPECT_FALSE(vcam::parse_bytes("5X", value));
}
