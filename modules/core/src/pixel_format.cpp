#include "vcam/core/pixel_format.hpp"

#include <algorithm>
#include <array>
#include <cctype>

namespace vcam {

namespace {

// One row per PixelFormat value, in enum order.
constexpr std::array<PixelFormatInfo, 7> kFormats = {{
    //  format              name     planes  yuv    even_w even_h
    {PixelFormat::YUYV,  "yuyv",  1,      true,  true,  false},
    {PixelFormat::UYVY,  "uyvy",  1,      true,  true,  false},
    {PixelFormat::I420,  "i420",  3,      true,  true,  true},
    {PixelFormat::NV12,  "nv12",  2,      true,  true,  true},
    {PixelFormat::RGB24, "rgb24", 1,      false, false, false},
    {PixelFormat::BGR24, "bgr24", 1,      false, false, false},
    {PixelFormat::GRAY8, "gray8", 1,      false, false, false},
}};

std::string to_lower(std::string_view text) {
    std::string lower(text);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower;
}

}  // namespace

const PixelFormatInfo& pixel_format_info(PixelFormat format) {
    return kFormats[static_cast<size_t>(format)];
}

const char* pixel_format_name(PixelFormat format) {
    return pixel_format_info(format).name;
}

Result<PixelFormat> parse_pixel_format(std::string_view text) {
    const std::string name = to_lower(text);

    for (const PixelFormatInfo& info : kFormats) {
        if (name == info.name) {
            return info.format;
        }
    }

    // Aliases used by other tools (V4L2 fourcc names, FFmpeg names, short forms).
    struct Alias { const char* alias; PixelFormat format; };
    static constexpr std::array<Alias, 9> kAliases = {{
        {"yuy2", PixelFormat::YUYV},    {"yuyv422", PixelFormat::YUYV},
        {"uyvy422", PixelFormat::UYVY},
        {"yu12", PixelFormat::I420},    {"yuv420p", PixelFormat::I420},
        {"rgb", PixelFormat::RGB24},    {"bgr", PixelFormat::BGR24},
        {"gray", PixelFormat::GRAY8},   {"grey", PixelFormat::GRAY8},
    }};
    for (const Alias& entry : kAliases) {
        if (name == entry.alias) {
            return entry.format;
        }
    }

    return Status(StatusCode::InvalidArgument,
                  "unknown pixel format '" + std::string(text) + "' (supported: " +
                      supported_pixel_format_names() + ")");
}

std::string supported_pixel_format_names() {
    std::string names;
    for (const PixelFormatInfo& info : kFormats) {
        if (!names.empty()) {
            names += ", ";
        }
        names += info.name;
    }
    return names;
}

const char* color_space_name(ColorSpace space) {
    return space == ColorSpace::BT709 ? "bt709" : "bt601";
}

const char* color_range_name(ColorRange range) {
    return range == ColorRange::Full ? "full" : "limited";
}

}  // namespace vcam
