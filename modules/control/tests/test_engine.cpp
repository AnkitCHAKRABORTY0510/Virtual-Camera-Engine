// End-to-end tests of the Engine WITHOUT a V4L2 device:
// pattern/video source -> buffer -> playhead -> scheduler -> raw file output.
// Most tests run twice: in stream mode (default, small decode-ahead window)
// and in ram mode (whole input decoded first) — the output must be identical.
// The pattern stamps each frame's number as a barcode, so reading the output
// file back proves exactly which source frame went into every output slot.
#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

#include "vcam/control/engine.hpp"
#include "vcam/core/log.hpp"
#include "vcam/source/frame_barcode.hpp"

using vcam::Config;
using vcam::Engine;

namespace vcam {
// Lets GoogleTest print the parameter as "stream"/"ram" instead of raw bytes.
void PrintTo(BufferMode mode, std::ostream* out) { *out << buffer_mode_name(mode); }
}  // namespace vcam

namespace {

std::string media(const std::string& name) {
    const char* directory = std::getenv("VCAM_TEST_MEDIA_DIR");
    return std::string(directory ? directory : "test_media") + "/" + name;
}

std::string temp_file(const std::string& name) {
    return (std::filesystem::temp_directory_path() / name).string();
}

// A fast-mode pattern run writing frames to a raw file.
Config pattern_config(const std::string& output, int64_t source_fps, int64_t output_fps, uint64_t frames,
                      vcam::BufferMode mode = vcam::BufferMode::Stream) {
    Config config;
    config.buffer.mode = mode;
    config.buffer.read_ahead_s = 0.1;  // stream mode: tiny window (5 frames at 30 fps) to stress it
    config.input.type = vcam::SourceType::Pattern;
    config.input.pattern_width = 128;
    config.input.pattern_height = 64;
    config.input.pattern_frames = frames;
    config.input.source_fps = vcam::make_rational(source_fps, 1);
    config.output.backend = vcam::OutputBackend::File;
    config.output.file_path = output;
    config.output.fps = vcam::make_rational(output_fps, 1);
    config.output.pixel_format = vcam::PixelFormat::YUYV;
    config.playback.realtime = false;
    config.playback.on_eof = vcam::EofPolicy::Stop;
    config.read_stdin_commands = false;
    return config;
}

// Reads the raw output file and returns the barcode of every frame.
std::vector<uint32_t> read_barcodes(const std::string& path, int width, int height, vcam::PixelFormat format) {
    const vcam::FrameFormat layout = vcam::make_frame_format(width, height, format).value();
    std::ifstream file(path, std::ios::binary);
    std::vector<uint8_t> bytes(layout.size_bytes);
    std::vector<uint32_t> codes;
    while (file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
        vcam::FrameView view{layout, std::span<const uint8_t>(bytes.data(), bytes.size()), {}};
        auto code = vcam::read_barcode(view);
        codes.push_back(code ? *code : 0xFFFFFFFF);
    }
    return codes;
}

std::vector<uint32_t> sequence(std::initializer_list<uint32_t> values) {
    return std::vector<uint32_t>(values);
}

class EngineTest : public ::testing::Test {
protected:
    void SetUp() override { vcam::log::set_level(vcam::LogLevel::Error); }
};

// Runs a test once per buffer mode (stream and ram).
class EngineModeTest : public EngineTest, public ::testing::WithParamInterface<vcam::BufferMode> {
protected:
    vcam::BufferMode mode() const { return GetParam(); }
    // Unique file name per mode, so the two runs never share an output file.
    std::string out_file(const std::string& name) const {
        return temp_file(std::string("vcam_") + vcam::buffer_mode_name(mode()) + "_" + name);
    }
};

}  // namespace

TEST_P(EngineModeTest, SameRateDeliversEveryFrameInOrder) {
    const std::string out = out_file("same.raw");
    Engine engine(pattern_config(out, 30, 30, 90, mode()));
    ASSERT_TRUE(engine.run_blocking(30).ok());
    const auto codes = read_barcodes(out, 128, 64, vcam::PixelFormat::YUYV);
    ASSERT_EQ(codes.size(), 90u);
    for (uint32_t i = 0; i < 90; ++i) {
        ASSERT_EQ(codes[i], i) << "slot " << i;
    }
    const vcam::TimingReport report = engine.timing_report();
    EXPECT_EQ(report.frames_published, 90u);
    EXPECT_EQ(report.lateness.max_ns, 0);  // simulated clock: exact
    EXPECT_EQ(engine.state(), vcam::EngineState::Stopped);
    std::filesystem::remove(out);
}

TEST_P(EngineModeTest, HalfRateDropsAndDoubleRateRepeats) {
    const std::string out = out_file("rates.raw");
    {
        Engine engine(pattern_config(out, 30, 15, 30, mode()));
        ASSERT_TRUE(engine.run_blocking(30).ok());
        const auto codes = read_barcodes(out, 128, 64, vcam::PixelFormat::YUYV);
        ASSERT_EQ(codes.size(), 15u);
        EXPECT_EQ(std::vector<uint32_t>(codes.begin(), codes.begin() + 4), sequence({0, 2, 4, 6}));
        EXPECT_EQ(engine.timing_report().source_frames_skipped, 14u);
    }
    {
        Engine engine(pattern_config(out, 15, 30, 10, mode()));
        ASSERT_TRUE(engine.run_blocking(30).ok());
        const auto codes = read_barcodes(out, 128, 64, vcam::PixelFormat::YUYV);
        ASSERT_EQ(codes.size(), 20u);
        EXPECT_EQ(std::vector<uint32_t>(codes.begin(), codes.begin() + 6), sequence({0, 0, 1, 1, 2, 2}));
        EXPECT_EQ(engine.timing_report().repeated_frames, 10u);
    }
    std::filesystem::remove(out);
}

TEST_P(EngineModeTest, LoopAndHoldWithFrameLimit) {
    const std::string out = out_file("loop.raw");
    Config loop = pattern_config(out, 30, 30, 5, mode());
    loop.playback.on_eof = vcam::EofPolicy::Loop;
    loop.playback.max_frames = 12;
    {
        Engine engine(loop);
        ASSERT_TRUE(engine.run_blocking(30).ok());
        EXPECT_EQ(read_barcodes(out, 128, 64, vcam::PixelFormat::YUYV),
                  sequence({0, 1, 2, 3, 4, 0, 1, 2, 3, 4, 0, 1}));
        EXPECT_EQ(engine.timing_report().loops, 2u);
    }
    Config hold = loop;
    hold.playback.on_eof = vcam::EofPolicy::Hold;
    hold.playback.max_frames = 8;
    {
        Engine engine(hold);
        ASSERT_TRUE(engine.run_blocking(30).ok());
        EXPECT_EQ(read_barcodes(out, 128, 64, vcam::PixelFormat::YUYV), sequence({0, 1, 2, 3, 4, 4, 4, 4}));
    }
    std::filesystem::remove(out);
}

TEST_F(EngineTest, DiskBufferAndOtherPixelFormat) {
    const std::string out = temp_file("vcam_engine_disk.raw");
    Config config = pattern_config(out, 30, 30, 40);
    config.buffer.mode = vcam::BufferMode::Disk;
    config.buffer.spill_dir = std::filesystem::temp_directory_path().string();
    config.output.pixel_format = vcam::PixelFormat::I420;
    Engine engine(config);
    ASSERT_TRUE(engine.run_blocking(30).ok());
    const auto codes = read_barcodes(out, 128, 64, vcam::PixelFormat::I420);
    ASSERT_EQ(codes.size(), 40u);
    EXPECT_EQ(codes[39], 39u);
    std::filesystem::remove(out);
}

TEST_P(EngineModeTest, VideoFileThroughTheWholePipeline) {
    if (!std::filesystem::exists(media("cfr30.mp4"))) {
        GTEST_SKIP() << "test media missing";
    }
    Config config;
    config.buffer.mode = mode();
    config.input.path = media("cfr30.mp4");
    config.output.backend = vcam::OutputBackend::Null;
    config.playback.realtime = false;
    config.playback.on_eof = vcam::EofPolicy::Stop;
    config.read_stdin_commands = false;
    Engine engine(config);
    ASSERT_TRUE(engine.run_blocking(60).ok());
    const vcam::EngineSnapshot snapshot = engine.snapshot();
    EXPECT_EQ(snapshot.frames_sent, 90u);                       // 3 s at 30 fps, fps taken from the file
    EXPECT_EQ(snapshot.output_fps, vcam::make_rational(30, 1));
    EXPECT_EQ(snapshot.output_format.width, 320);               // resolution = source resolution
    EXPECT_NEAR(snapshot.duration_s, 3.0, 1e-9);
}

TEST_P(EngineModeTest, RealTimePauseResumeAndSeek) {
    Config config = pattern_config("", 30, 50, 500, mode());  // 50 fps output, 10 s pattern at 30 fps
    config.output.backend = vcam::OutputBackend::Null;
    config.playback.realtime = true;
    config.playback.on_eof = vcam::EofPolicy::Loop;
    Engine engine(config);
    ASSERT_TRUE(engine.start().ok());
    auto wait_for = [&](vcam::EngineState wanted) {
        for (int i = 0; i < 500 && engine.state() != wanted; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return engine.state() == wanted;
    };
    ASSERT_TRUE(wait_for(vcam::EngineState::Ready));
    ASSERT_TRUE(engine.start_streaming().ok());
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    engine.post({vcam::CommandType::Pause, 0});
    ASSERT_TRUE(wait_for(vcam::EngineState::Paused));
    const double paused_at = engine.snapshot().position_s;
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_NEAR(engine.snapshot().position_s, paused_at, 1e-9);  // source time frozen
    const uint64_t sent_while_paused = engine.snapshot().frames_sent;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_GT(engine.snapshot().frames_sent, sent_while_paused);   // camera keeps delivering (hold)

    engine.post({vcam::CommandType::Resume, 0});
    ASSERT_TRUE(wait_for(vcam::EngineState::Streaming));
    engine.post({vcam::CommandType::Seek, 7.0});
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_NEAR(engine.snapshot().position_s, 7.0, 0.3);

    engine.request_stop();
    engine.shutdown();
    EXPECT_EQ(engine.state(), vcam::EngineState::Stopped);
    const vcam::TimingReport report = engine.timing_report();
    EXPECT_GT(report.frames_published, 20u);
    EXPECT_NEAR(report.measured_fps, 50.0, 2.0);
}

TEST_F(EngineTest, MissingInputFailsWithInputExitCode) {
    Config config;
    config.input.path = "/no/such/video.mp4";
    config.output.backend = vcam::OutputBackend::Null;
    config.read_stdin_commands = false;
    Engine engine(config);
    EXPECT_FALSE(engine.run_blocking(10).ok());
    EXPECT_EQ(engine.state(), vcam::EngineState::Error);
    EXPECT_EQ(engine.exit_code(), vcam::kExitInput);
}

TEST_F(EngineTest, MissingDeviceFailsFastWithDeviceExitCode) {
    Config config = pattern_config("", 30, 30, 10);
    config.output.backend = vcam::OutputBackend::V4L2;
    config.output.device = "/dev/video_not_there";
    Engine engine(config);
    EXPECT_FALSE(engine.run_blocking(10).ok());
    EXPECT_EQ(engine.exit_code(), vcam::kExitDevice);
    EXPECT_NE(engine.snapshot().error.find("v4l2loopback"), std::string::npos);
}

TEST_F(EngineTest, MemoryBudgetIsCheckedBeforeLoading) {
    Config config = pattern_config("", 30, 30, 1000, vcam::BufferMode::Ram);
    config.output.backend = vcam::OutputBackend::Null;
    config.buffer.memory_limit = 1 << 20;  // 1 MiB for 1000 frames of 16 KiB
    Engine engine(config);
    EXPECT_FALSE(engine.run_blocking(10).ok());
    EXPECT_EQ(engine.exit_code(), vcam::kExitRuntime);
    EXPECT_NE(engine.snapshot().error.find("--buffer-mode disk"), std::string::npos);
}

TEST_F(EngineTest, StopDuringBufferingIsClean) {
    // 100 000 frames would take minutes to buffer; the budget check passes
    // (the limit is high) but we stop after 200 ms, long before memory fills.
    Config config = pattern_config("", 30, 30, 100'000, vcam::BufferMode::Ram);
    config.output.backend = vcam::OutputBackend::Null;
    config.input.pattern_width = 640;
    config.input.pattern_height = 360;
    config.buffer.memory_limit = 64ull << 30;
    Engine engine(config);
    ASSERT_TRUE(engine.start().ok());
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    engine.request_stop();
    engine.shutdown();
    EXPECT_EQ(engine.state(), vcam::EngineState::Stopped);
}

// ---------------------------------------------------------------------------
// Stream mode only
// ---------------------------------------------------------------------------

TEST_F(EngineTest, StreamModeUsesASmallFixedWindow) {
    // 3000 frames of 640x360 would need ~1.3 GiB in ram mode; the stream
    // window holds read_ahead × fps + 2 frames, whatever the length.
    Config config = pattern_config("", 30, 30, 3000);
    config.output.backend = vcam::OutputBackend::Null;
    config.input.pattern_width = 640;
    config.input.pattern_height = 360;
    config.buffer.read_ahead_s = 1.0;
    config.playback.max_frames = 120;
    Engine engine(config);
    ASSERT_TRUE(engine.run_blocking(30).ok());
    const vcam::EngineSnapshot snapshot = engine.snapshot();
    EXPECT_EQ(snapshot.buffer.kind, "stream");
    EXPECT_EQ(snapshot.buffer.capacity_frames, 32u);  // 1 s × 30 fps + 2
    EXPECT_LT(snapshot.buffer.memory_bytes, 32u * 640 * 360 * 2 + 32u * 64);
    EXPECT_EQ(snapshot.frames_sent, 120u);
}

TEST_F(EngineTest, StreamModeLoopSeamIsExact) {
    // 90-frame pattern, looped, through a 5-frame window: the seam must go
    // 88, 89, 0, 1 with no repeat or skip, and be counted once per pass.
    const std::string out = temp_file("vcam_stream_seam.raw");
    Config config = pattern_config(out, 30, 30, 90);
    config.playback.on_eof = vcam::EofPolicy::Loop;
    config.playback.max_frames = 200;
    Engine engine(config);
    ASSERT_TRUE(engine.run_blocking(30).ok());
    const auto codes = read_barcodes(out, 128, 64, vcam::PixelFormat::YUYV);
    ASSERT_EQ(codes.size(), 200u);
    for (uint32_t i = 0; i < 200; ++i) {
        ASSERT_EQ(codes[i], i % 90) << "slot " << i;
    }
    const vcam::TimingReport report = engine.timing_report();
    EXPECT_EQ(report.loops, 2u);
    EXPECT_EQ(report.repeated_frames, 0u);
    EXPECT_EQ(report.source_frames_skipped, 0u);
    std::filesystem::remove(out);
}

TEST_F(EngineTest, StreamModeRealTimeSeekShowsTheTargetFrame) {
    // Real time, file output: after "seek 7" the next new frame must be
    // pattern frame 210 (7 s × 30 fps), with the old frame held meanwhile.
    const std::string out = temp_file("vcam_stream_seek.raw");
    Config config = pattern_config(out, 30, 30, 300);
    config.playback.realtime = true;
    config.playback.on_eof = vcam::EofPolicy::Loop;
    Engine engine(config);
    ASSERT_TRUE(engine.start().ok());
    for (int i = 0; i < 500 && engine.state() != vcam::EngineState::Ready; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_TRUE(engine.start_streaming().ok());
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    engine.post({vcam::CommandType::Seek, 7.0});
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_NEAR(engine.snapshot().position_s, 7.0 + 0.3, 0.15);
    engine.request_stop();
    engine.shutdown();

    const auto codes = read_barcodes(out, 128, 64, vcam::PixelFormat::YUYV);
    ASSERT_GT(codes.size(), 10u);
    size_t jump = 0;
    while (jump < codes.size() && codes[jump] < 100) {
        ++jump;
    }
    ASSERT_LT(jump, codes.size()) << "the seek never reached the screen";
    EXPECT_EQ(codes[jump], 210u);
    for (size_t i = jump + 1; i < codes.size(); ++i) {
        EXPECT_GE(codes[i], codes[i - 1]) << "slot " << i;  // playing on from 210, in order
    }
    std::filesystem::remove(out);
}

TEST_F(EngineTest, StreamModeStopDuringBufferingIsClean) {
    Config config = pattern_config("", 30, 30, 100'000);
    config.output.backend = vcam::OutputBackend::Null;
    Engine engine(config);
    ASSERT_TRUE(engine.start().ok());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    engine.request_stop();
    engine.shutdown();
    EXPECT_EQ(engine.state(), vcam::EngineState::Stopped);
}

INSTANTIATE_TEST_SUITE_P(BufferModes, EngineModeTest,
                         ::testing::Values(vcam::BufferMode::Stream, vcam::BufferMode::Ram),
                         [](const ::testing::TestParamInfo<vcam::BufferMode>& param_info) {
                             return std::string(vcam::buffer_mode_name(param_info.param));
                         });
