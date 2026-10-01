// =============================================================================
// format_converter.hpp — decoded frame (any FFmpeg pixel format) -> engine format
//
// Responsibility: turn one decoded FFmpeg frame into the engine's output pixel
// format, writing straight into memory owned by the caller (e.g. a buffer slot).
//
// It NEVER scales. The architecture rule (v0.2) is "output resolution = input
// resolution", so a size mismatch is reported as an error.
//
// What it does do:
//   * pixel format conversion   e.g. yuv420p -> YUYV, yuv444p -> RGB24
//   * colour matrix conversion  e.g. BT.709 (HD files) -> BT.601 (webcam convention)
//   * range conversion          e.g. full-range JPEG-style YUV -> limited range
//   * plain copy fast path      when input and output are already identical
//
// FFmpeg types are only forward-declared here, so modules that include this
// header do not need FFmpeg's headers.
// =============================================================================
#pragma once

#include <cstdint>

#include "vcam/core/frame.hpp"
#include "vcam/core/status.hpp"

struct AVFrame;     // FFmpeg decoded frame (libavutil/frame.h)
struct SwsContext;  // libswscale conversion context (libswscale/swscale.h)

namespace vcam {

// Colour signalling of a decoded frame.
struct SourceColorimetry {
    bool is_yuv = true;
    ColorSpace space = ColorSpace::BT601;
    ColorRange range = ColorRange::Limited;
    bool space_guessed = false;  // file did not say; guessed from frame height (>= 720 -> BT.709)
};

// Reads colour space / range from the frame's metadata (with the usual
// fallbacks when the file does not specify them).
SourceColorimetry detect_colorimetry(const AVFrame& frame);

// FFmpeg's AVPixelFormat value for one of our pixel formats (returned as int
// so this header does not need FFmpeg's enum).
int to_ffmpeg_pixel_format(PixelFormat format);

class FormatConverter {
public:
    // Which method the last convert() used (useful for tests and diagnostics).
    enum class Path { None, Copy, Swscale };

    explicit FormatConverter(const FrameFormat& output_format);
    ~FormatConverter();

    // Owns a native SwsContext: copying would double-free it.
    FormatConverter(const FormatConverter&) = delete;
    FormatConverter& operator=(const FormatConverter&) = delete;

    // Converts `input` into `output`. `output.format` must equal the format
    // given to the constructor and `input` must have the same width/height.
    Status convert(const AVFrame& input, MutableFrameView output);

    // Same, for a frame already in one of the engine's own pixel formats
    // (used by generated sources such as the test pattern and script input).
    Status convert(const FrameView& input, MutableFrameView output);

    const FrameFormat& output_format() const { return output_format_; }
    Path last_path() const { return last_path_; }

private:
    // True when bytes can be copied as-is (same pixel format and colour signalling).
    bool can_copy(int input_av_format, const SourceColorimetry& input_colors) const;

    // Plane pointers + strides + format of an input image, whatever its origin.
    struct InputPlanes {
        const uint8_t* data[4] = {nullptr, nullptr, nullptr, nullptr};
        int linesize[4] = {0, 0, 0, 0};
        int av_format = -1;
        int width = 0;
        int height = 0;
        SourceColorimetry colors;
    };

    // Shared implementation of both convert() overloads.
    Status convert_planes(const InputPlanes& input, MutableFrameView output);

    // Copies every plane row by row (handles different strides).
    void copy_planes(const InputPlanes& input, MutableFrameView output) const;

    // (Re)creates the swscale context only when the input format/colours change.
    Status prepare_swscale(int input_av_format, const SourceColorimetry& input_colors);

    FrameFormat output_format_;
    int output_av_format_;

    SwsContext* sws_context_ = nullptr;
    int cached_input_av_format_ = -1;
    SourceColorimetry cached_input_colors_{};

    Path last_path_ = Path::None;
};

const char* converter_path_name(FormatConverter::Path path);

}  // namespace vcam
