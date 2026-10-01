// Unit tests for FormatConverter: pixel format, colour matrix, range, copy path.
//
// Reference colour values (8-bit, limited range) for pure red:
//   BT.709: Y=63  Cb=102 Cr=240
//   BT.601: Y=81  Cb=90  Cr=240
// So converting BT.709 red to BT.601 must change Y from 63 to ~81 and Cb from
// 102 to ~90. If the matrix were ignored, the values would stay 63/102.
#include <gtest/gtest.h>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/log.h>
#include <libavutil/pixfmt.h>
}

#include <cstdlib>
#include <cstring>  // std::memset
#include <memory>

#include "vcam/convert/format_converter.hpp"

using vcam::ColorRange;
using vcam::ColorSpace;
using vcam::FormatConverter;
using vcam::PixelFormat;

namespace {

// unique_ptr with av_frame_free as deleter, so frames are freed automatically.
struct AvFrameDeleter {
    void operator()(AVFrame* frame) const { av_frame_free(&frame); }
};
using AvFramePtr = std::unique_ptr<AVFrame, AvFrameDeleter>;

// Allocates an FFmpeg frame and fills every plane-0/1/2 sample with Y/U/V.
AvFramePtr make_yuv_frame(AVPixelFormat format, int width, int height, uint8_t y, uint8_t u, uint8_t v,
                          AVColorSpace space, AVColorRange range) {
    AvFramePtr frame(av_frame_alloc());
    frame->format = format;
    frame->width = width;
    frame->height = height;
    frame->colorspace = space;
    frame->color_range = range;
    // av_frame_get_buffer allocates data[] planes for the format/size set above.
    EXPECT_EQ(av_frame_get_buffer(frame.get(), 0), 0);

    const int chroma_height = (format == AV_PIX_FMT_YUV444P) ? height : (height + 1) / 2;
    const int chroma_width = (format == AV_PIX_FMT_YUV444P) ? width : (width + 1) / 2;
    for (int row = 0; row < height; ++row) {
        std::memset(frame->data[0] + row * frame->linesize[0], y, static_cast<size_t>(width));
    }
    for (int row = 0; row < chroma_height; ++row) {
        std::memset(frame->data[1] + row * frame->linesize[1], u, static_cast<size_t>(chroma_width));
        std::memset(frame->data[2] + row * frame->linesize[2], v, static_cast<size_t>(chroma_width));
    }
    return frame;
}

vcam::FrameFormat output_format(int width, int height, PixelFormat format) {
    auto result = vcam::make_frame_format(width, height, format, ColorSpace::BT601, ColorRange::Limited);
    EXPECT_TRUE(result.ok()) << result.status().to_string();
    return result.value();
}

int at(const vcam::OwnedFrame& frame, size_t offset) {
    return frame.view().bytes[offset];
}

}  // namespace

TEST(FormatConverter, Bt709ToBt601WhenFormatsDiffer) {
    auto input = make_yuv_frame(AV_PIX_FMT_YUV420P, 64, 32, 63, 102, 240, AVCOL_SPC_BT709, AVCOL_RANGE_MPEG);
    FormatConverter converter(output_format(64, 32, PixelFormat::YUYV));
    vcam::OwnedFrame output(converter.output_format());

    ASSERT_TRUE(converter.convert(*input, output.mutable_view()).ok());
    EXPECT_EQ(converter.last_path(), FormatConverter::Path::Swscale);
    // YUYV byte order: Y0 U Y1 V
    EXPECT_NEAR(at(output, 0), 81, 2);
    EXPECT_NEAR(at(output, 1), 90, 2);
    EXPECT_NEAR(at(output, 3), 240, 2);
}

TEST(FormatConverter, Bt709ToBt601WhenFormatsAreTheSame) {
    // Same pixel format (yuv420p -> I420) but different matrix: must NOT be a plain copy.
    auto input = make_yuv_frame(AV_PIX_FMT_YUV420P, 64, 32, 63, 102, 240, AVCOL_SPC_BT709, AVCOL_RANGE_MPEG);
    FormatConverter converter(output_format(64, 32, PixelFormat::I420));
    vcam::OwnedFrame output(converter.output_format());

    ASSERT_TRUE(converter.convert(*input, output.mutable_view()).ok());
    EXPECT_EQ(converter.last_path(), FormatConverter::Path::Swscale);
    const auto& layout = converter.output_format();
    EXPECT_NEAR(at(output, layout.planes[0].offset), 81, 2);
    EXPECT_NEAR(at(output, layout.planes[1].offset), 90, 2);
    EXPECT_NEAR(at(output, layout.planes[2].offset), 240, 2);
}

TEST(FormatConverter, IdenticalFormatUsesBitExactCopy) {
    auto input = make_yuv_frame(AV_PIX_FMT_YUV420P, 64, 32, 81, 90, 240, AVCOL_SPC_SMPTE170M, AVCOL_RANGE_MPEG);
    // Make one sample unique to prove the data really moved.
    input->data[0][5] = 17;
    FormatConverter converter(output_format(64, 32, PixelFormat::I420));
    vcam::OwnedFrame output(converter.output_format());

    ASSERT_TRUE(converter.convert(*input, output.mutable_view()).ok());
    EXPECT_EQ(converter.last_path(), FormatConverter::Path::Copy);
    EXPECT_EQ(at(output, 5), 17);
    EXPECT_EQ(at(output, converter.output_format().planes[1].offset), 90);
}

TEST(FormatConverter, RgbAndBgrChannelOrder) {
    auto input = make_yuv_frame(AV_PIX_FMT_YUV420P, 64, 32, 81, 90, 240, AVCOL_SPC_SMPTE170M, AVCOL_RANGE_MPEG);

    FormatConverter to_rgb(output_format(64, 32, PixelFormat::RGB24));
    vcam::OwnedFrame rgb(to_rgb.output_format());
    ASSERT_TRUE(to_rgb.convert(*input, rgb.mutable_view()).ok());
    EXPECT_NEAR(at(rgb, 0), 255, 3);  // R
    EXPECT_NEAR(at(rgb, 1), 0, 3);    // G
    EXPECT_NEAR(at(rgb, 2), 0, 3);    // B

    FormatConverter to_bgr(output_format(64, 32, PixelFormat::BGR24));
    vcam::OwnedFrame bgr(to_bgr.output_format());
    ASSERT_TRUE(to_bgr.convert(*input, bgr.mutable_view()).ok());
    EXPECT_NEAR(at(bgr, 0), 0, 3);    // B
    EXPECT_NEAR(at(bgr, 2), 255, 3);  // R
}

TEST(FormatConverter, FullRangeInputIsCompressedToLimited) {
    // yuvj420p is deprecated in FFmpeg; silence its "deprecated pixel format" notice.
    av_log_set_level(AV_LOG_ERROR);
    // yuvj420p = full range: Y 255 is peak white, which is 235 in limited range.
    auto input = make_yuv_frame(AV_PIX_FMT_YUVJ420P, 64, 32, 255, 128, 128, AVCOL_SPC_SMPTE170M, AVCOL_RANGE_JPEG);
    FormatConverter converter(output_format(64, 32, PixelFormat::YUYV));
    vcam::OwnedFrame output(converter.output_format());
    ASSERT_TRUE(converter.convert(*input, output.mutable_view()).ok());
    EXPECT_NEAR(at(output, 0), 235, 1);
    EXPECT_NEAR(at(output, 1), 128, 1);
}

TEST(FormatConverter, OddSizeWorksWithRgb) {
    auto input = make_yuv_frame(AV_PIX_FMT_YUV444P, 321, 241, 81, 90, 240, AVCOL_SPC_SMPTE170M, AVCOL_RANGE_MPEG);
    FormatConverter converter(output_format(321, 241, PixelFormat::RGB24));
    vcam::OwnedFrame output(converter.output_format());
    ASSERT_TRUE(converter.convert(*input, output.mutable_view()).ok());
    const size_t last_pixel = converter.output_format().size_bytes - 3;
    EXPECT_NEAR(at(output, last_pixel), 255, 3);
}

TEST(FormatConverter, RejectsResolutionChange) {
    auto input = make_yuv_frame(AV_PIX_FMT_YUV420P, 128, 64, 81, 90, 240, AVCOL_SPC_SMPTE170M, AVCOL_RANGE_MPEG);
    FormatConverter converter(output_format(64, 32, PixelFormat::YUYV));
    vcam::OwnedFrame output(converter.output_format());
    vcam::Status status = converter.convert(*input, output.mutable_view());
    EXPECT_EQ(status.code(), vcam::StatusCode::Unsupported);
}

TEST(Colorimetry, GuessesMatrixFromHeightWhenUnspecified) {
    auto hd = make_yuv_frame(AV_PIX_FMT_YUV420P, 1280, 720, 0, 0, 0, AVCOL_SPC_UNSPECIFIED, AVCOL_RANGE_UNSPECIFIED);
    auto sd = make_yuv_frame(AV_PIX_FMT_YUV420P, 640, 480, 0, 0, 0, AVCOL_SPC_UNSPECIFIED, AVCOL_RANGE_UNSPECIFIED);
    EXPECT_EQ(vcam::detect_colorimetry(*hd).space, ColorSpace::BT709);
    EXPECT_TRUE(vcam::detect_colorimetry(*hd).space_guessed);
    EXPECT_EQ(vcam::detect_colorimetry(*sd).space, ColorSpace::BT601);
    EXPECT_EQ(vcam::detect_colorimetry(*sd).range, ColorRange::Limited);
}

TEST(FormatConverter, ConvertsEngineFramesToo) {
    // RGB24 red -> YUYV (BT.601 limited): Y~81, U~90, V~240.
    auto rgb_format = vcam::make_frame_format(32, 16, PixelFormat::RGB24).value();
    vcam::OwnedFrame rgb(rgb_format);
    auto view = rgb.mutable_view();
    for (size_t i = 0; i < view.bytes.size(); i += 3) {
        view.bytes[i] = 255;
        view.bytes[i + 1] = 0;
        view.bytes[i + 2] = 0;
    }
    FormatConverter converter(output_format(32, 16, PixelFormat::YUYV));
    vcam::OwnedFrame output(converter.output_format());
    ASSERT_TRUE(converter.convert(rgb.view(), output.mutable_view()).ok());
    EXPECT_NEAR(at(output, 0), 81, 2);
    EXPECT_NEAR(at(output, 1), 90, 2);
    EXPECT_NEAR(at(output, 3), 240, 2);

    // Same format in and out: plain copy.
    FormatConverter same(rgb_format);
    vcam::OwnedFrame copy(rgb_format);
    ASSERT_TRUE(same.convert(rgb.view(), copy.mutable_view()).ok());
    EXPECT_EQ(same.last_path(), FormatConverter::Path::Copy);
    EXPECT_EQ(at(copy, 0), 255);
}
