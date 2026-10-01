#include "vcam/convert/format_converter.hpp"

// FFmpeg is a C library: its headers must be wrapped in extern "C" so the
// C++ compiler does not mangle the function names.
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>  // av_image_copy_plane
#include <libavutil/pixdesc.h>   // av_pix_fmt_desc_get, av_get_pix_fmt_name
#include <libswscale/swscale.h>
}

#include "vcam/core/log.hpp"

namespace vcam {

namespace {

constexpr const char* kModule = "convert";

bool is_full_range_yuvj(AVPixelFormat format) {
    // The deprecated "yuvj" formats are YUV with full (JPEG) range.
    return format == AV_PIX_FMT_YUVJ420P || format == AV_PIX_FMT_YUVJ422P ||
           format == AV_PIX_FMT_YUVJ444P || format == AV_PIX_FMT_YUVJ440P;
}

// libswscale identifies colour matrices with SWS_CS_* constants.
int to_sws_colorspace(ColorSpace space) {
    return space == ColorSpace::BT709 ? SWS_CS_ITU709 : SWS_CS_ITU601;
}

}  // namespace

SourceColorimetry detect_colorimetry(const AVFrame& frame) {
    SourceColorimetry colors;
    const auto pixel_format = static_cast<AVPixelFormat>(frame.format);

    // av_pix_fmt_desc_get describes a pixel format (planes, flags, bit depth).
    const AVPixFmtDescriptor* descriptor = av_pix_fmt_desc_get(pixel_format);
    // Palette formats (PAL8, e.g. some GIFs) store RGB colours in a palette.
    const bool is_rgb = descriptor != nullptr &&
                        (descriptor->flags & (AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_PAL)) != 0;
    const bool is_gray = descriptor != nullptr && descriptor->nb_components <= 2 && !is_rgb;
    colors.is_yuv = !is_rgb && !is_gray;

    if (!colors.is_yuv) {
        colors.range = ColorRange::Full;
        return colors;
    }

    switch (frame.colorspace) {
        case AVCOL_SPC_BT709:
            colors.space = ColorSpace::BT709;
            break;
        case AVCOL_SPC_BT470BG:    // PAL BT.601
        case AVCOL_SPC_SMPTE170M:  // NTSC BT.601
        case AVCOL_SPC_FCC:
            colors.space = ColorSpace::BT601;
            break;
        default:
            // Unspecified (or a matrix we do not convert exactly, e.g. BT.2020):
            // follow the common convention that HD content uses BT.709.
            colors.space = frame.height >= 720 ? ColorSpace::BT709 : ColorSpace::BT601;
            colors.space_guessed = true;
            break;
    }

    if (frame.color_range == AVCOL_RANGE_JPEG || is_full_range_yuvj(pixel_format)) {
        colors.range = ColorRange::Full;
    } else {
        colors.range = ColorRange::Limited;  // MPEG range is the default for video
    }
    return colors;
}

int to_ffmpeg_pixel_format(PixelFormat format) {
    switch (format) {
        case PixelFormat::YUYV:  return AV_PIX_FMT_YUYV422;
        case PixelFormat::UYVY:  return AV_PIX_FMT_UYVY422;
        case PixelFormat::I420:  return AV_PIX_FMT_YUV420P;
        case PixelFormat::NV12:  return AV_PIX_FMT_NV12;
        case PixelFormat::RGB24: return AV_PIX_FMT_RGB24;
        case PixelFormat::BGR24: return AV_PIX_FMT_BGR24;
        case PixelFormat::GRAY8: return AV_PIX_FMT_GRAY8;
    }
    return AV_PIX_FMT_NONE;
}

const char* converter_path_name(FormatConverter::Path path) {
    switch (path) {
        case FormatConverter::Path::None:    return "none";
        case FormatConverter::Path::Copy:    return "copy";
        case FormatConverter::Path::Swscale: return "swscale";
    }
    return "?";
}

FormatConverter::FormatConverter(const FrameFormat& output_format)
    : output_format_(output_format), output_av_format_(to_ffmpeg_pixel_format(output_format.pixel_format)) {}

FormatConverter::~FormatConverter() {
    // sws_freeContext accepts nullptr.
    sws_freeContext(sws_context_);
}

bool FormatConverter::can_copy(int input_av_format, const SourceColorimetry& input_colors) const {
    if (input_av_format != output_av_format_) {
        return false;
    }
    if (!pixel_format_info(output_format_.pixel_format).is_yuv) {
        return true;  // RGB/GRAY: same format means same meaning
    }
    // Same YUV layout, but the numbers only mean the same colours if the
    // matrix and range match too.
    return input_colors.space == output_format_.color_space && input_colors.range == output_format_.color_range;
}

void FormatConverter::copy_planes(const InputPlanes& input, MutableFrameView output) const {
    for (int plane = 0; plane < output_format_.plane_count; ++plane) {
        const PlaneLayout& layout = output_format_.planes[static_cast<size_t>(plane)];
        // av_image_copy_plane copies `rows` rows of `row_bytes` bytes, stepping
        // each side by its own stride (FFmpeg rows are usually padded).
        av_image_copy_plane(output.plane(plane), static_cast<int>(layout.stride_bytes),
                            input.data[plane], input.linesize[plane],
                            static_cast<int>(layout.row_bytes), static_cast<int>(layout.rows));
    }
}

Status FormatConverter::prepare_swscale(int input_av_format, const SourceColorimetry& input_colors) {
    const bool unchanged = sws_context_ != nullptr && input_av_format == cached_input_av_format_ &&
                           input_colors.space == cached_input_colors_.space &&
                           input_colors.range == cached_input_colors_.range;
    if (unchanged) {
        return Status::ok_status();
    }

    // sws_getCachedContext reuses `sws_context_` when the parameters did not
    // change, otherwise frees it and creates a new one.
    // Source and destination sizes are identical: no scaling happens, the
    // filter flag only affects chroma up/down-sampling.
    // SWS_ACCURATE_RND is deliberately NOT used: it disables libswscale's SIMD
    // paths and measured 2-3x slower at 1080p (54 -> 119 fps for YUYV) while
    // colours stay within +-2 levels without it (see test_format_converter).
    sws_context_ = sws_getCachedContext(sws_context_,
                                        output_format_.width, output_format_.height,
                                        static_cast<AVPixelFormat>(input_av_format),
                                        output_format_.width, output_format_.height,
                                        static_cast<AVPixelFormat>(output_av_format_),
                                        SWS_BILINEAR,
                                        nullptr, nullptr, nullptr);
    if (sws_context_ == nullptr) {
        const char* name = av_get_pix_fmt_name(static_cast<AVPixelFormat>(input_av_format));
        return Status(StatusCode::Unsupported,
                      std::string("cannot convert pixel format ") + (name ? name : "?") + " to " +
                          pixel_format_name(output_format_.pixel_format));
    }

    // Tell swscale which colour matrix / range the input uses and which the
    // output must use. sws_getCoefficients returns the matrix table for a
    // SWS_CS_* constant. Brightness 0, contrast and saturation 1.0 (= 1 << 16
    // in swscale's 16.16 fixed point) mean "no adjustment".
    const bool output_is_yuv = pixel_format_info(output_format_.pixel_format).is_yuv;
    const int* input_table = sws_getCoefficients(to_sws_colorspace(input_colors.space));
    const int* output_table = sws_getCoefficients(to_sws_colorspace(output_format_.color_space));
    const int input_full_range = input_colors.range == ColorRange::Full ? 1 : 0;
    const int output_full_range = (!output_is_yuv || output_format_.color_range == ColorRange::Full) ? 1 : 0;
    sws_setColorspaceDetails(sws_context_, input_table, input_full_range, output_table, output_full_range,
                             0, 1 << 16, 1 << 16);

    cached_input_av_format_ = input_av_format;
    cached_input_colors_ = input_colors;

    VCAM_DEBUG(kModule, "swscale context: "
                            << av_get_pix_fmt_name(static_cast<AVPixelFormat>(input_av_format)) << " "
                            << color_space_name(input_colors.space) << "/" << color_range_name(input_colors.range)
                            << (input_colors.space_guessed ? " (matrix guessed)" : "") << " -> "
                            << describe(output_format_));
    return Status::ok_status();
}

Status FormatConverter::convert(const AVFrame& input, MutableFrameView output) {
    if (input.format < 0) {
        return Status(StatusCode::InvalidData, "decoded frame has no pixel format");
    }
    InputPlanes planes;
    for (int i = 0; i < 4; ++i) {
        planes.data[i] = input.data[i];
        planes.linesize[i] = input.linesize[i];
    }
    planes.av_format = input.format;
    planes.width = input.width;
    planes.height = input.height;
    planes.colors = detect_colorimetry(input);
    return convert_planes(planes, output);
}

Status FormatConverter::convert(const FrameView& input, MutableFrameView output) {
    InputPlanes planes;
    for (int i = 0; i < input.format.plane_count; ++i) {
        planes.data[i] = input.plane(i);
        planes.linesize[i] = static_cast<int>(input.format.planes[static_cast<size_t>(i)].stride_bytes);
    }
    planes.av_format = to_ffmpeg_pixel_format(input.format.pixel_format);
    planes.width = input.format.width;
    planes.height = input.format.height;
    planes.colors.is_yuv = pixel_format_info(input.format.pixel_format).is_yuv;
    planes.colors.space = input.format.color_space;
    planes.colors.range = input.format.color_range;
    return convert_planes(planes, output);
}

Status FormatConverter::convert_planes(const InputPlanes& input, MutableFrameView output) {
    if (!same_layout(output.format, output_format_) || output.bytes.size() < output_format_.size_bytes) {
        return Status(StatusCode::InvalidArgument, "output view does not match the converter's format");
    }
    if (input.width != output_format_.width || input.height != output_format_.height) {
        // No scaling by design (output resolution = input resolution).
        return Status(StatusCode::Unsupported,
                      "frame size changed mid-stream from " + std::to_string(output_format_.width) + "x" +
                          std::to_string(output_format_.height) + " to " + std::to_string(input.width) + "x" +
                          std::to_string(input.height) + " (resolution changes are not supported)");
    }

    if (can_copy(input.av_format, input.colors)) {
        copy_planes(input, output);
        last_path_ = Path::Copy;
        return Status::ok_status();
    }

    Status status = prepare_swscale(input.av_format, input.colors);
    if (!status.ok()) {
        return status;
    }

    // Destination plane pointers and strides in the form swscale expects.
    uint8_t* destination[4] = {nullptr, nullptr, nullptr, nullptr};
    int destination_stride[4] = {0, 0, 0, 0};
    for (int plane = 0; plane < output_format_.plane_count; ++plane) {
        destination[plane] = output.plane(plane);
        destination_stride[plane] = static_cast<int>(output_format_.planes[static_cast<size_t>(plane)].stride_bytes);
    }

    // sws_scale converts rows [0, height) of the input and returns the number
    // of output rows written.
    const int rows_written = sws_scale(sws_context_, input.data, input.linesize, 0, input.height,
                                       destination, destination_stride);
    if (rows_written != output_format_.height) {
        return Status(StatusCode::Internal, "sws_scale wrote " + std::to_string(rows_written) + " rows, expected " +
                                                std::to_string(output_format_.height));
    }
    last_path_ = Path::Swscale;
    return Status::ok_status();
}

}  // namespace vcam
