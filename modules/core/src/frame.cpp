#include "vcam/core/frame.hpp"

#include <cstdlib>  // std::aligned_alloc, std::free
#include <new>      // std::bad_alloc

namespace vcam {

namespace {

// Largest size we accept. Keeps every byte count far away from overflow and
// rejects obviously broken metadata (8K is 7680x4320).
constexpr int kMaxDimension = 16384;

// Alignment used for owned frame memory; matches the widest common SIMD
// registers (AVX-512 = 64 bytes).
constexpr size_t kFrameAlignment = 64;

// Fills one plane and returns the offset right after it.
size_t add_plane(FrameFormat& format, int index, size_t offset, size_t row_bytes, size_t rows) {
    PlaneLayout& plane = format.planes[static_cast<size_t>(index)];
    plane.offset = offset;
    plane.row_bytes = row_bytes;
    plane.stride_bytes = row_bytes;  // tightly packed: no padding at the end of rows
    plane.rows = rows;
    return offset + row_bytes * rows;
}

}  // namespace

Result<FrameFormat> make_frame_format(int width, int height, PixelFormat pixel_format,
                                      ColorSpace color_space, ColorRange color_range) {
    if (width <= 0 || height <= 0 || width > kMaxDimension || height > kMaxDimension) {
        return Status(StatusCode::InvalidArgument,
                      "invalid frame size " + std::to_string(width) + "x" + std::to_string(height));
    }

    const PixelFormatInfo& info = pixel_format_info(pixel_format);
    if ((info.needs_even_width && width % 2 != 0) || (info.needs_even_height && height % 2 != 0)) {
        return Status(StatusCode::InvalidArgument,
                      std::string("pixel format ") + info.name + " cannot represent " +
                          std::to_string(width) + "x" + std::to_string(height) + " (needs even " +
                          (info.needs_even_height ? "width and height" : "width") +
                          "); use rgb24, bgr24 or gray8 for this source");
    }

    FrameFormat format;
    format.width = width;
    format.height = height;
    format.pixel_format = pixel_format;
    format.plane_count = info.plane_count;
    format.color_space = color_space;
    // RGB and grey frames are always full range; the matrix is irrelevant for them.
    format.color_range = info.is_yuv ? color_range : ColorRange::Full;

    const size_t w = static_cast<size_t>(width);
    const size_t h = static_cast<size_t>(height);
    size_t end = 0;

    switch (pixel_format) {
        case PixelFormat::YUYV:
        case PixelFormat::UYVY:
            end = add_plane(format, 0, 0, w * 2, h);
            break;
        case PixelFormat::I420:
            end = add_plane(format, 0, 0, w, h);              // Y  : full resolution
            end = add_plane(format, 1, end, w / 2, h / 2);    // U  : half width, half height
            end = add_plane(format, 2, end, w / 2, h / 2);    // V  : half width, half height
            break;
        case PixelFormat::NV12:
            end = add_plane(format, 0, 0, w, h);              // Y
            end = add_plane(format, 1, end, w, h / 2);        // UV interleaved: half height
            break;
        case PixelFormat::RGB24:
        case PixelFormat::BGR24:
            end = add_plane(format, 0, 0, w * 3, h);
            break;
        case PixelFormat::GRAY8:
            end = add_plane(format, 0, 0, w, h);
            break;
    }

    format.size_bytes = end;
    return format;
}

std::string describe(const FrameFormat& format) {
    std::string text = std::to_string(format.width) + "x" + std::to_string(format.height) + " " +
                       pixel_format_name(format.pixel_format);
    if (pixel_format_info(format.pixel_format).is_yuv) {
        text += std::string(" ") + color_space_name(format.color_space) + "/" +
                color_range_name(format.color_range);
    }
    text += " (" + std::to_string(format.size_bytes) + " bytes/frame)";
    return text;
}

bool same_layout(const FrameFormat& a, const FrameFormat& b) {
    return a.width == b.width && a.height == b.height && a.pixel_format == b.pixel_format &&
           a.size_bytes == b.size_bytes;
}

void OwnedFrame::FreeDeleter::operator()(uint8_t* pointer) const {
    std::free(pointer);
}

OwnedFrame::OwnedFrame(const FrameFormat& format) : format_(format) {
    // std::aligned_alloc requires the size to be a multiple of the alignment.
    size_t padded = (format.size_bytes + kFrameAlignment - 1) / kFrameAlignment * kFrameAlignment;
    void* memory = std::aligned_alloc(kFrameAlignment, padded);
    if (memory == nullptr) {
        throw std::bad_alloc();
    }
    data_.reset(static_cast<uint8_t*>(memory));
}

MutableFrameView OwnedFrame::mutable_view() {
    return MutableFrameView{format_, std::span<uint8_t>(data_.get(), format_.size_bytes)};
}

FrameView OwnedFrame::view(const FrameTiming& timing) const {
    return FrameView{format_, std::span<const uint8_t>(data_.get(), format_.size_bytes), timing};
}

}  // namespace vcam
