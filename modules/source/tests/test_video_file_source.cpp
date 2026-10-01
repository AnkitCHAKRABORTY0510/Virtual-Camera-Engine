// Tests for VideoFileSource against the generated media in $VCAM_TEST_MEDIA_DIR
// (created by tools/gen_test_media.sh; CTest runs it automatically first).
//
// Run alone:  ctest --test-dir build -L source --output-on-failure
// or by hand: VCAM_TEST_MEDIA_DIR=build/test_media build/bin/test_source
#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "vcam/core/log.hpp"
#include "vcam/source/timestamp_stats.hpp"
#include "vcam/source/video_file_source.hpp"

using vcam::PixelFormat;
using vcam::StatusCode;

namespace {

std::string media(const std::string& name) {
    const char* directory = std::getenv("VCAM_TEST_MEDIA_DIR");
    return std::string(directory ? directory : "test_media") + "/" + name;
}

// Skips the current test when the media directory is missing (e.g. the test
// binary was started by hand without generating media first).
#define REQUIRE_MEDIA(name)                                                              \
    do {                                                                                 \
        if (!std::filesystem::exists(media(name))) {                                     \
            GTEST_SKIP() << "missing " << media(name)                                    \
                         << " (run: bash tools/gen_test_media.sh build/test_media)";     \
        }                                                                                \
    } while (0)

// Everything a test wants to know after decoding a whole file.
struct DecodeOutcome {
    vcam::Status open_status;
    vcam::Status format_status;
    vcam::SourceInfo info;
    std::vector<vcam::FrameTiming> timings;
    vcam::SourceStats stats;
    bool ended_with_error = false;
    vcam::Status final_status;
    std::vector<uint8_t> first_frame_bytes;
};

DecodeOutcome decode_all(const std::string& path, PixelFormat pixel_format = PixelFormat::YUYV) {
    DecodeOutcome outcome;
    vcam::VideoFileSource source(path);
    outcome.open_status = source.open();
    if (!outcome.open_status.ok()) {
        return outcome;
    }
    outcome.info = source.info();

    auto format = vcam::make_output_format(source.info(), pixel_format);
    if (!format.ok()) {
        outcome.format_status = format.status();
        return outcome;
    }
    outcome.format_status = source.set_output_format(format.value());
    if (!outcome.format_status.ok()) {
        return outcome;
    }

    vcam::OwnedFrame frame(format.value());
    while (true) {
        vcam::FrameTiming timing;
        vcam::ReadResult result = source.read_next(frame.mutable_view(), timing);
        if (result.is_end_of_stream()) {
            break;
        }
        if (result.is_error()) {
            outcome.ended_with_error = true;
            outcome.final_status = result.status();
            break;
        }
        if (outcome.timings.empty()) {
            auto bytes = frame.view().bytes;
            outcome.first_frame_bytes.assign(bytes.begin(), bytes.end());
        }
        outcome.timings.push_back(timing);
    }
    outcome.stats = source.stats();
    source.close();
    return outcome;
}

class VideoFileSourceTest : public ::testing::Test {
protected:
    void SetUp() override { vcam::log::set_level(vcam::LogLevel::Error); }  // keep test output clean
};

}  // namespace

// ---- Opening -------------------------------------------------------------------

TEST_F(VideoFileSourceTest, MissingFileIsNotFound) {
    vcam::VideoFileSource source("/definitely/not/here.mp4");
    vcam::Status status = source.open();
    EXPECT_EQ(status.code(), StatusCode::NotFound) << status.to_string();
}

TEST_F(VideoFileSourceTest, TextFileIsRejected) {
    REQUIRE_MEDIA("not_a_video.txt");
    vcam::VideoFileSource source(media("not_a_video.txt"));
    vcam::Status status = source.open();
    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.code(), StatusCode::InvalidData) << status.to_string();
}

TEST_F(VideoFileSourceTest, AudioOnlyFileHasNoVideoStream) {
    REQUIRE_MEDIA("audio_only.m4a");
    vcam::VideoFileSource source(media("audio_only.m4a"));
    vcam::Status status = source.open();
    EXPECT_EQ(status.code(), StatusCode::Unsupported) << status.to_string();
}

TEST_F(VideoFileSourceTest, ReadBeforeSetOutputFormatIsAnError) {
    REQUIRE_MEDIA("cfr30.mp4");
    vcam::VideoFileSource source(media("cfr30.mp4"));
    ASSERT_TRUE(source.open().ok());
    vcam::FrameTiming timing;
    EXPECT_TRUE(source.read_next(vcam::MutableFrameView{}, timing).is_error());
}

TEST_F(VideoFileSourceTest, OutputSizeMustEqualSourceSize) {
    REQUIRE_MEDIA("cfr30.mp4");
    vcam::VideoFileSource source(media("cfr30.mp4"));
    ASSERT_TRUE(source.open().ok());
    auto wrong = vcam::make_frame_format(640, 480, PixelFormat::YUYV);
    EXPECT_EQ(source.set_output_format(wrong.value()).code(), StatusCode::InvalidArgument);
}

// ---- Metadata --------------------------------------------------------------------

TEST_F(VideoFileSourceTest, Cfr30Metadata) {
    REQUIRE_MEDIA("cfr30.mp4");
    vcam::VideoFileSource source(media("cfr30.mp4"));
    ASSERT_TRUE(source.open().ok());
    const vcam::SourceInfo& info = source.info();
    EXPECT_EQ(info.width, 320);
    EXPECT_EQ(info.height, 240);
    ASSERT_TRUE(info.nominal_fps.has_value());
    EXPECT_EQ(*info.nominal_fps, vcam::make_rational(30, 1));
    EXPECT_TRUE(vcam::is_positive(info.time_base));
    ASSERT_TRUE(info.frame_count_estimate.has_value());
    EXPECT_EQ(*info.frame_count_estimate, 90u);
    EXPECT_FALSE(info.is_live);
}

// ---- Decoding: count, order, timestamps -------------------------------------------------

// Checks that frame i is shown exactly at i / fps seconds (exact rational comparison).
void expect_exact_cfr_timestamps(const DecodeOutcome& outcome, vcam::Rational fps) {
    for (size_t i = 0; i < outcome.timings.size(); ++i) {
        const vcam::FrameTiming& timing = outcome.timings[i];
        EXPECT_EQ(timing.source_index, i);
        // pts * time_base == i / fps   <=>   pts * tb.num * fps.num == i * tb.den * fps.den
        const vcam::Rational pts_seconds = vcam::make_rational(timing.pts_ticks * outcome.info.time_base.num,
                                                               outcome.info.time_base.den);
        const vcam::Rational expected = vcam::make_rational(static_cast<int64_t>(i) * fps.den, fps.num);
        ASSERT_EQ(vcam::compare(pts_seconds, expected), 0)
            << "frame " << i << " pts " << timing.pts_ticks << " ticks";
    }
}

TEST_F(VideoFileSourceTest, DecodesAllFramesOfCfrFilesWithExactTimestamps) {
    struct Case { const char* file; int64_t fps_num; int64_t fps_den; size_t frames; };
    const Case cases[] = {
        {"cfr30.mp4", 30, 1, 90},
        {"cfr15.mp4", 15, 1, 45},
        {"cfr24.mp4", 24, 1, 72},
        {"cfr60.mp4", 60, 1, 180},
        {"ntsc2997.mp4", 30000, 1001, 90},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.file);
        REQUIRE_MEDIA(c.file);
        DecodeOutcome outcome = decode_all(media(c.file));
        ASSERT_TRUE(outcome.open_status.ok()) << outcome.open_status.to_string();
        ASSERT_FALSE(outcome.ended_with_error) << outcome.final_status.to_string();
        EXPECT_EQ(outcome.timings.size(), c.frames);
        EXPECT_EQ(outcome.stats.frames_decoded, c.frames);
        EXPECT_EQ(outcome.stats.decode_errors, 0u);
        EXPECT_EQ(outcome.stats.timestamp_repairs, 0u);
        expect_exact_cfr_timestamps(outcome, vcam::make_rational(c.fps_num, c.fps_den));
    }
}

TEST_F(VideoFileSourceTest, TimestampsStrictlyIncreaseDespiteBFrames) {
    REQUIRE_MEDIA("cfr30.mp4");
    DecodeOutcome outcome = decode_all(media("cfr30.mp4"));
    ASSERT_GE(outcome.timings.size(), 2u);
    EXPECT_EQ(outcome.timings.front().pts_ticks, 0);  // normalised start
    for (size_t i = 1; i < outcome.timings.size(); ++i) {
        EXPECT_GT(outcome.timings[i].pts_ticks, outcome.timings[i - 1].pts_ticks) << "frame " << i;
    }
}

TEST_F(VideoFileSourceTest, FrameContentIsWritten) {
    REQUIRE_MEDIA("cfr30.mp4");
    DecodeOutcome outcome = decode_all(media("cfr30.mp4"));
    ASSERT_FALSE(outcome.first_frame_bytes.empty());
    // testsrc2 is colourful: the frame must contain more than one byte value.
    const uint8_t first = outcome.first_frame_bytes.front();
    bool varied = false;
    for (uint8_t byte : outcome.first_frame_bytes) {
        if (byte != first) { varied = true; break; }
    }
    EXPECT_TRUE(varied);
}

TEST_F(VideoFileSourceTest, VariableFrameRateIsDetected) {
    REQUIRE_MEDIA("vfr.mkv");
    DecodeOutcome outcome = decode_all(media("vfr.mkv"));
    ASSERT_TRUE(outcome.open_status.ok()) << outcome.open_status.to_string();
    EXPECT_EQ(outcome.timings.size(), 60u);

    vcam::TimestampStats stats;
    for (const auto& timing : outcome.timings) {
        stats.add(timing.pts_ticks);
    }
    // Container time base is 1 ms: allow 1 tick of rounding, still clearly VFR.
    EXPECT_FALSE(stats.is_constant_frame_rate(1));
    EXPECT_GE(stats.max_interval(), 2 * stats.min_interval() - 2);
}

TEST_F(VideoFileSourceTest, AllPixelFormatsDecode) {
    REQUIRE_MEDIA("cfr15.mp4");
    for (PixelFormat format : {PixelFormat::YUYV, PixelFormat::UYVY, PixelFormat::I420, PixelFormat::NV12,
                               PixelFormat::RGB24, PixelFormat::BGR24, PixelFormat::GRAY8}) {
        SCOPED_TRACE(vcam::pixel_format_name(format));
        DecodeOutcome outcome = decode_all(media("cfr15.mp4"), format);
        ASSERT_TRUE(outcome.format_status.ok()) << outcome.format_status.to_string();
        EXPECT_EQ(outcome.timings.size(), 45u);
    }
}

TEST_F(VideoFileSourceTest, HdBt709FileDecodes) {
    REQUIRE_MEDIA("hd709.mp4");
    DecodeOutcome outcome = decode_all(media("hd709.mp4"));
    EXPECT_EQ(outcome.info.width, 1280);
    EXPECT_EQ(outcome.info.height, 720);
    EXPECT_EQ(outcome.timings.size(), 15u);
}

// ---- Resolution rule ------------------------------------------------------------------

TEST_F(VideoFileSourceTest, OddSizeRejectedForYuyvButWorksForRgb) {
    REQUIRE_MEDIA("odd.mkv");
    DecodeOutcome yuyv = decode_all(media("odd.mkv"), PixelFormat::YUYV);
    ASSERT_TRUE(yuyv.open_status.ok());
    EXPECT_EQ(yuyv.format_status.code(), StatusCode::InvalidArgument);

    DecodeOutcome rgb = decode_all(media("odd.mkv"), PixelFormat::RGB24);
    ASSERT_TRUE(rgb.format_status.ok()) << rgb.format_status.to_string();
    EXPECT_EQ(rgb.info.width, 321);
    EXPECT_EQ(rgb.info.height, 241);
    EXPECT_EQ(rgb.timings.size(), 10u);
}

// ---- Damaged input --------------------------------------------------------------------

TEST_F(VideoFileSourceTest, TruncatedFileDecodesWhatIsThere) {
    REQUIRE_MEDIA("truncated.ts");
    DecodeOutcome outcome = decode_all(media("truncated.ts"));
    ASSERT_TRUE(outcome.open_status.ok()) << outcome.open_status.to_string();
    EXPECT_FALSE(outcome.ended_with_error) << outcome.final_status.to_string();
    EXPECT_GT(outcome.timings.size(), 10u);
    EXPECT_LT(outcome.timings.size(), 90u);
}

TEST_F(VideoFileSourceTest, CorruptedFileDoesNotStopDecoding) {
    REQUIRE_MEDIA("corrupt.ts");
    DecodeOutcome outcome = decode_all(media("corrupt.ts"));
    ASSERT_TRUE(outcome.open_status.ok()) << outcome.open_status.to_string();
    EXPECT_FALSE(outcome.ended_with_error) << outcome.final_status.to_string();
    // Most frames survive; the damaged region is skipped or concealed.
    EXPECT_GT(outcome.timings.size(), 60u);
    for (size_t i = 1; i < outcome.timings.size(); ++i) {
        EXPECT_GT(outcome.timings[i].pts_ticks, outcome.timings[i - 1].pts_ticks);
    }
}

// ---- Lifecycle -----------------------------------------------------------------------

TEST_F(VideoFileSourceTest, CloseIsIdempotentAndReopenRestarts) {
    REQUIRE_MEDIA("cfr15.mp4");
    vcam::VideoFileSource source(media("cfr15.mp4"));
    ASSERT_TRUE(source.open().ok());
    source.close();
    source.close();  // second close must be harmless

    ASSERT_TRUE(source.open().ok());
    auto format = vcam::make_output_format(source.info(), PixelFormat::I420);
    ASSERT_TRUE(source.set_output_format(format.value()).ok());
    vcam::OwnedFrame frame(format.value());
    vcam::FrameTiming timing;
    ASSERT_TRUE(source.read_next(frame.mutable_view(), timing).is_frame());
    EXPECT_EQ(timing.source_index, 0u);
    EXPECT_EQ(timing.pts_ticks, 0);
}

TEST_F(VideoFileSourceTest, EndOfStreamIsSticky) {
    REQUIRE_MEDIA("cfr15.mp4");
    vcam::VideoFileSource source(media("cfr15.mp4"));
    ASSERT_TRUE(source.open().ok());
    auto format = vcam::make_output_format(source.info(), PixelFormat::GRAY8);
    ASSERT_TRUE(source.set_output_format(format.value()).ok());
    vcam::OwnedFrame frame(format.value());
    vcam::FrameTiming timing;
    while (source.read_next(frame.mutable_view(), timing).is_frame()) {
    }
    EXPECT_TRUE(source.read_next(frame.mutable_view(), timing).is_end_of_stream());
    EXPECT_TRUE(source.read_next(frame.mutable_view(), timing).is_end_of_stream());
}

// ---- Seeking (used for looping and the 'seek' command in stream mode) ---------------

namespace {
// Seeks, reads one frame, returns its pts (or -1).
int64_t pts_after_seek(vcam::VideoFileSource& source, vcam::OwnedFrame& frame, int64_t target) {
    if (!source.seek(target).ok()) return -1;
    vcam::FrameTiming timing;
    return source.read_next(frame.mutable_view(), timing).is_frame() ? timing.pts_ticks : -1;
}
}  // namespace

TEST_F(VideoFileSourceTest, SeekLandsOnTheFrameShownAtTheTarget) {
    for (const char* file : {"cfr30.mp4", "cfr30.ts", "vfr.mkv"}) {
        SCOPED_TRACE(file);
        REQUIRE_MEDIA(file);
        // Reference: every frame's pts from a full decode.
        DecodeOutcome reference = decode_all(media(file));
        ASSERT_GE(reference.timings.size(), 40u);

        vcam::VideoFileSource source(media(file));
        ASSERT_TRUE(source.open().ok());
        auto format = vcam::make_output_format(source.info(), PixelFormat::GRAY8).value();
        ASSERT_TRUE(source.set_output_format(format).ok());
        vcam::OwnedFrame frame(format);

        // Exactly on frame 30, between frames 30 and 31, backwards to 5, and to 0.
        for (size_t index : {30u, 5u, 0u, 39u}) {
            const int64_t exact = reference.timings[index].pts_ticks;
            EXPECT_EQ(pts_after_seek(source, frame, exact), exact) << "frame " << index;
            const int64_t inside = exact + 1;  // one tick into the frame: still that frame
            EXPECT_EQ(pts_after_seek(source, frame, inside), exact) << "frame " << index;
        }
    }
}

TEST_F(VideoFileSourceTest, SeekAfterEndOfStreamRestartsDecoding) {
    REQUIRE_MEDIA("cfr15.mp4");
    vcam::VideoFileSource source(media("cfr15.mp4"));
    ASSERT_TRUE(source.open().ok());
    auto format = vcam::make_output_format(source.info(), PixelFormat::YUYV).value();
    ASSERT_TRUE(source.set_output_format(format).ok());
    vcam::OwnedFrame frame(format);
    vcam::FrameTiming timing;
    for (int lap = 0; lap < 3; ++lap) {  // play, rewind, play again...
        int count = 0;
        while (source.read_next(frame.mutable_view(), timing).is_frame()) {
            ++count;
        }
        EXPECT_EQ(count, 45) << "lap " << lap;
        ASSERT_TRUE(source.seek(0).ok());
    }
}

TEST_F(VideoFileSourceTest, LimitedDecoderThreadsStillDecode) {
    REQUIRE_MEDIA("cfr30.mp4");
    vcam::VideoFileSource source(media("cfr30.mp4"));
    source.set_decoder_threads(1);
    ASSERT_TRUE(source.open().ok());
    auto format = vcam::make_output_format(source.info(), PixelFormat::YUYV).value();
    ASSERT_TRUE(source.set_output_format(format).ok());
    vcam::OwnedFrame frame(format);
    vcam::FrameTiming timing;
    int count = 0;
    while (source.read_next(frame.mutable_view(), timing).is_frame()) {
        ++count;
    }
    EXPECT_EQ(count, 90);
}
