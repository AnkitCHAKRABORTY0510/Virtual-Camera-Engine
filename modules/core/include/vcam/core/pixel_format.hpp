// =============================================================================
// pixel_format.hpp — the pixel formats the engine can output, and their layout
//
// This file answers: "given width, height and a pixel format, where does each
// byte of the frame live?" The answer is a FrameFormat (see frame.hpp) with a
// list of planes, each with an offset, a stride (bytes per row) and a row count.
//
// All layouts are TIGHTLY PACKED (stride == visible bytes per row, planes stored
// back to back) because that is what V4L2 consumers expect from a webcam.
// Full byte-level description of every format: docs/FORMATS.md.
// =============================================================================
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include "vcam/core/status.hpp"

namespace vcam {

enum class PixelFormat {
    YUYV,   // 4:2:2 packed, bytes: Y0 U Y1 V              (2 bytes/pixel)  — default
    UYVY,   // 4:2:2 packed, bytes: U Y0 V Y1              (2 bytes/pixel)
    I420,   // 4:2:0 planar: Y plane, then U, then V       (1.5 bytes/pixel) aka YU12
    NV12,   // 4:2:0 semi-planar: Y plane, then interleaved UV (1.5 bytes/pixel)
    RGB24,  // packed R G B                                (3 bytes/pixel)
    BGR24,  // packed B G R (OpenCV's native order)        (3 bytes/pixel)
    GRAY8,  // luma only                                   (1 byte/pixel)
};

// YUV formats carry a colour matrix (BT.601/BT.709) and a range; RGB/GRAY do not
// need a matrix.
enum class ColorSpace {
    BT601,  // SD / conventional webcam signalling
    BT709,  // HD
};

enum class ColorRange {
    Limited,  // "TV range": Y 16..235, UV 16..240
    Full,     // "PC range": 0..255
};

// Static facts about one pixel format.
struct PixelFormatInfo {
    PixelFormat format;
    const char* name;          // lower-case CLI name, e.g. "yuyv"
    int plane_count;           // 1 for packed formats, 2 for NV12, 3 for I420
    bool is_yuv;               // needs a colour matrix
    bool needs_even_width;     // horizontal chroma subsampling
    bool needs_even_height;    // vertical chroma subsampling
};

// Returns the info record for a format (never fails: every enum value has one).
const PixelFormatInfo& pixel_format_info(PixelFormat format);

// "yuyv", "i420", ... (lower case).
const char* pixel_format_name(PixelFormat format);

// Accepts the CLI names case-insensitively plus common aliases:
// "yuy2" -> YUYV, "yu12"/"yuv420p" -> I420, "rgb" -> RGB24, "bgr" -> BGR24, "gray" -> GRAY8.
Result<PixelFormat> parse_pixel_format(std::string_view text);

// Comma-separated list of supported names, for help/error messages.
std::string supported_pixel_format_names();

const char* color_space_name(ColorSpace space);
const char* color_range_name(ColorRange range);

}  // namespace vcam
