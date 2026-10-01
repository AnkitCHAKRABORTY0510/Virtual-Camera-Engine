#include "vcam/source/pattern_source.hpp"

#include <algorithm>
#include <array>
#include <cstring>

#include "vcam/convert/format_converter.hpp"
#include "vcam/source/frame_barcode.hpp"

namespace vcam {

namespace {

// Classic 75 % colour bars (R, G, B).
constexpr std::array<std::array<uint8_t, 3>, 8> kBars = {{
    {191, 191, 191}, {191, 191, 0}, {0, 191, 191}, {0, 191, 0},
    {191, 0, 191},   {191, 0, 0},   {0, 0, 191},   {16, 16, 16},
}};

}  // namespace

PatternSource::PatternSource(PatternOptions options) : options_(options) {}
PatternSource::~PatternSource() = default;

Status PatternSource::open() {
    if (options_.width < kBarcodeMinWidth || options_.height < 16) {
        return Status(StatusCode::InvalidArgument, "pattern size must be at least 64x16");
    }
    if (!is_positive(options_.fps) || options_.frame_count == 0) {
        return Status(StatusCode::InvalidArgument, "pattern needs a positive fps and frame count");
    }
    info_ = SourceInfo{};
    info_.uri = "pattern";
    info_.container_name = "generated";
    info_.codec_name = "test pattern";
    info_.native_pixel_format = "rgb24";
    info_.width = options_.width;
    info_.height = options_.height;
    // One tick per frame: time base = 1 / fps, frame i has pts i.
    info_.time_base = invert(options_.fps);
    info_.nominal_fps = options_.fps;
    info_.frame_count_estimate = options_.frame_count;
    info_.duration_ticks = static_cast<int64_t>(options_.frame_count);
    info_.is_seekable = true;

    auto rgb = make_frame_format(options_.width, options_.height, PixelFormat::RGB24).value();
    background_ = OwnedFrame(rgb);
    canvas_ = OwnedFrame(rgb);

    // Draw the colour bars once; every frame starts as a copy of them.
    MutableFrameView view = background_.mutable_view();
    const size_t stride = rgb.planes[0].stride_bytes;
    for (int y = 0; y < options_.height; ++y) {
        uint8_t* row = view.bytes.data() + static_cast<size_t>(y) * stride;
        for (int x = 0; x < options_.width; ++x) {
            const auto bar = static_cast<size_t>(x * 8 / options_.width);
            std::memcpy(row + static_cast<size_t>(x) * 3, kBars[bar].data(), 3);
        }
    }
    next_index_ = 0;
    stats_ = SourceStats{};
    return Status::ok_status();
}

Status PatternSource::set_output_format(const FrameFormat& format) {
    if (format.width != options_.width || format.height != options_.height) {
        return Status(StatusCode::InvalidArgument, "output size must equal the pattern size");
    }
    converter_ = std::make_unique<FormatConverter>(format);
    return Status::ok_status();
}

ReadResult PatternSource::read_next(MutableFrameView destination, FrameTiming& timing) {
    if (!converter_) {
        return ReadResult::error(Status(StatusCode::InvalidArgument, "set_output_format() not called"));
    }
    if (next_index_ >= options_.frame_count) {
        return ReadResult::end_of_stream();
    }
    const uint64_t index = next_index_++;

    MutableFrameView canvas = canvas_.mutable_view();
    std::memcpy(canvas.bytes.data(), background_.view().bytes.data(), canvas.bytes.size());
    const size_t stride = canvas.format.planes[0].stride_bytes;

    // Moving white box: advances 8 px per frame and wraps, in the middle band.
    const int box = std::max(8, options_.height / 6);
    const int travel = std::max(1, options_.width - box);
    const int box_x = static_cast<int>((index * 8) % static_cast<uint64_t>(travel));
    const int box_y = (options_.height - box) / 2;
    for (int y = box_y; y < box_y + box; ++y) {
        std::memset(canvas.bytes.data() + static_cast<size_t>(y) * stride + static_cast<size_t>(box_x) * 3, 255,
                    static_cast<size_t>(box) * 3);
    }

    draw_barcode_rgb(canvas.bytes.data(), stride, options_.width, options_.height, static_cast<uint32_t>(index));

    Status converted = converter_->convert(canvas_.view(), destination);
    if (!converted.ok()) {
        return ReadResult::error(converted);
    }
    timing.source_index = index;
    timing.pts_ticks = static_cast<int64_t>(index);
    timing.duration_ticks = 1;
    ++stats_.frames_decoded;
    return ReadResult::frame();
}

Status PatternSource::seek(int64_t pts_ticks) {
    const auto last = static_cast<int64_t>(options_.frame_count) - 1;
    next_index_ = static_cast<uint64_t>(std::clamp<int64_t>(pts_ticks, 0, last));
    return Status::ok_status();
}

void PatternSource::close() {
    converter_.reset();
}

}  // namespace vcam
