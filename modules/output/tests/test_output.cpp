// Unit tests for the output backends. The V4L2 tests exercise every error path
// that does not need a real v4l2loopback device; tests/e2e covers the device.
#include <gtest/gtest.h>

#include <linux/videodev2.h>

#include <cstring>
#include <filesystem>
#include <fstream>

#include "vcam/output/v4l2_loopback_camera.hpp"
#include "vcam/output/virtual_camera.hpp"

TEST(Fourcc, MappingAndText) {
    EXPECT_EQ(vcam::v4l2_fourcc_for(vcam::PixelFormat::YUYV), V4L2_PIX_FMT_YUYV);
    EXPECT_EQ(vcam::v4l2_fourcc_for(vcam::PixelFormat::I420), V4L2_PIX_FMT_YUV420);
    EXPECT_EQ(vcam::fourcc_to_string(V4L2_PIX_FMT_YUYV), "YUYV");
    EXPECT_EQ(vcam::fourcc_to_string(V4L2_PIX_FMT_YUV420), "YU12");
    EXPECT_EQ(vcam::fourcc_to_string(V4L2_PIX_FMT_BGR24), "BGR3");
}

TEST(V4L2Probe, MissingDeviceExplainsSetup) {
    auto result = vcam::V4L2LoopbackCamera::probe("/dev/video_does_not_exist");
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), vcam::StatusCode::NotFound);
    EXPECT_NE(result.status().message().find("v4l2loopback"), std::string::npos);
}

TEST(V4L2Probe, RegularFileIsRejected) {
    const auto path = std::filesystem::temp_directory_path() / "vcam_not_a_device";
    std::ofstream(path) << "x";
    auto result = vcam::V4L2LoopbackCamera::probe(path.string());
    EXPECT_EQ(result.status().code(), vcam::StatusCode::DeviceError);
    std::filesystem::remove(path);
}

TEST(V4L2Probe, NonVideoCharacterDeviceIsRejected) {
    // /dev/null is a character device but answers VIDIOC_QUERYCAP with an error.
    auto result = vcam::V4L2LoopbackCamera::probe("/dev/null");
    EXPECT_EQ(result.status().code(), vcam::StatusCode::DeviceError);
    EXPECT_NE(result.status().message().find("not a V4L2"), std::string::npos);
}

TEST(NullCamera, CountsFrames) {
    vcam::NullCamera camera;
    auto format = vcam::make_frame_format(64, 64, vcam::PixelFormat::GRAY8).value();
    ASSERT_TRUE(camera.open(format, vcam::make_rational(30, 1)).ok());
    vcam::OwnedFrame frame(format);
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(camera.publish(frame.view()).kind, vcam::PublishOutcome::Kind::Ok);
    }
    EXPECT_EQ(camera.frames_published(), 5u);
}

TEST(RawFileCamera, WritesFramesBackToBack) {
    const auto path = std::filesystem::temp_directory_path() / "vcam_raw_output.bin";
    auto format = vcam::make_frame_format(16, 8, vcam::PixelFormat::GRAY8).value();
    {
        vcam::RawFileCamera camera(path.string());
        ASSERT_TRUE(camera.open(format, vcam::make_rational(30, 1)).ok());
        vcam::OwnedFrame frame(format);
        for (uint8_t value = 0; value < 3; ++value) {
            auto view = frame.mutable_view();
            std::memset(view.bytes.data(), value, view.bytes.size());
            ASSERT_EQ(camera.publish(frame.view()).kind, vcam::PublishOutcome::Kind::Ok);
        }
    }
    EXPECT_EQ(std::filesystem::file_size(path), 3u * 128u);
    std::ifstream in(path, std::ios::binary);
    std::vector<char> bytes(3 * 128);
    in.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    EXPECT_EQ(bytes[0], 0);
    EXPECT_EQ(bytes[128], 1);
    EXPECT_EQ(bytes[256], 2);
    std::filesystem::remove(path);
}

TEST(RawFileCamera, UnwritablePathFails) {
    vcam::RawFileCamera camera("/proc/definitely/not/writable.raw");
    auto format = vcam::make_frame_format(16, 8, vcam::PixelFormat::GRAY8).value();
    EXPECT_EQ(camera.open(format, vcam::make_rational(30, 1)).code(), vcam::StatusCode::IoError);
}
