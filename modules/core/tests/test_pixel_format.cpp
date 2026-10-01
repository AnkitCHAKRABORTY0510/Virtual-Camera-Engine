// Unit tests for pixel-format parsing and frame layout computation.
#include <gtest/gtest.h>

#include "vcam/core/frame.hpp"
#include "vcam/core/pixel_format.hpp"

using vcam::PixelFormat;

TEST(PixelFormat, ParseNamesAndAliases) {
    EXPECT_EQ(vcam::parse_pixel_format("yuyv").value(), PixelFormat::YUYV);
    EXPECT_EQ(vcam::parse_pixel_format("YUY2").value(), PixelFormat::YUYV);
    EXPECT_EQ(vcam::parse_pixel_format("yuv420p").value(), PixelFormat::I420);
    EXPECT_EQ(vcam::parse_pixel_format("BGR").value(), PixelFormat::BGR24);
    EXPECT_FALSE(vcam::parse_pixel_format("mjpeg").ok());
}

TEST(FrameFormat, YuyvLayout720p) {
    auto format = vcam::make_frame_format(1280, 720, PixelFormat::YUYV);
    ASSERT_TRUE(format.ok());
    EXPECT_EQ(format->plane_count, 1);
    EXPECT_EQ(format->planes[0].stride_bytes, 2560u);
    EXPECT_EQ(format->size_bytes, 1280u * 720u * 2u);  // 1 843 200 bytes
}

TEST(FrameFormat, I420PlanesAreContiguous) {
    auto format = vcam::make_frame_format(1280, 720, PixelFormat::I420);
    ASSERT_TRUE(format.ok());
    ASSERT_EQ(format->plane_count, 3);
    EXPECT_EQ(format->planes[0].offset, 0u);
    EXPECT_EQ(format->planes[1].offset, 1280u * 720u);
    EXPECT_EQ(format->planes[1].stride_bytes, 640u);
    EXPECT_EQ(format->planes[2].offset, 1280u * 720u + 640u * 360u);
    EXPECT_EQ(format->size_bytes, 1280u * 720u * 3u / 2u);
}

TEST(FrameFormat, Nv12Layout) {
    auto format = vcam::make_frame_format(640, 480, PixelFormat::NV12);
    ASSERT_TRUE(format.ok());
    ASSERT_EQ(format->plane_count, 2);
    EXPECT_EQ(format->planes[1].offset, 640u * 480u);
    EXPECT_EQ(format->planes[1].stride_bytes, 640u);
    EXPECT_EQ(format->planes[1].rows, 240u);
    EXPECT_EQ(format->size_bytes, 640u * 480u * 3u / 2u);
}

TEST(FrameFormat, PackedRgbAndGray) {
    EXPECT_EQ(vcam::make_frame_format(321, 241, PixelFormat::RGB24)->size_bytes, 321u * 241u * 3u);
    EXPECT_EQ(vcam::make_frame_format(321, 241, PixelFormat::GRAY8)->size_bytes, 321u * 241u);
    // RGB is always full range, whatever the caller asked for.
    EXPECT_EQ(vcam::make_frame_format(64, 64, PixelFormat::RGB24, vcam::ColorSpace::BT601,
                                      vcam::ColorRange::Limited)->color_range,
              vcam::ColorRange::Full);
}

TEST(FrameFormat, RejectsSizesTheFormatCannotRepresent) {
    EXPECT_FALSE(vcam::make_frame_format(321, 240, PixelFormat::YUYV).ok());  // odd width
    EXPECT_TRUE(vcam::make_frame_format(320, 241, PixelFormat::YUYV).ok());   // odd height is fine for 4:2:2
    EXPECT_FALSE(vcam::make_frame_format(320, 241, PixelFormat::I420).ok());  // 4:2:0 needs even height
    EXPECT_FALSE(vcam::make_frame_format(0, 240, PixelFormat::RGB24).ok());
    EXPECT_FALSE(vcam::make_frame_format(20000, 240, PixelFormat::RGB24).ok());
}

TEST(OwnedFrame, AllocatesAlignedMemoryOfTheRightSize) {
    auto format = vcam::make_frame_format(1920, 1080, PixelFormat::I420);
    ASSERT_TRUE(format.ok());
    vcam::OwnedFrame frame(format.value());
    auto view = frame.mutable_view();
    EXPECT_EQ(view.bytes.size(), format->size_bytes);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(view.bytes.data()) % 64u, 0u);
    // Plane pointers follow the layout offsets.
    EXPECT_EQ(view.plane(1), view.bytes.data() + format->planes[1].offset);
}
