#include "vcam/source/image_sequence_source.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>

#include "vcam/source/video_file_source.hpp"

namespace vcam {

namespace {

bool is_image_file(const std::filesystem::path& path) {
    static constexpr std::array<const char*, 8> kExtensions = {".jpg", ".jpeg", ".png", ".bmp",
                                                               ".tif", ".tiff", ".webp", ".ppm"};
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return std::find(kExtensions.begin(), kExtensions.end(), extension) != kExtensions.end();
}

}  // namespace

bool natural_less(const std::string& a, const std::string& b) {
    size_t i = 0;
    size_t j = 0;
    while (i < a.size() && j < b.size()) {
        const bool digit_a = std::isdigit(static_cast<unsigned char>(a[i])) != 0;
        const bool digit_b = std::isdigit(static_cast<unsigned char>(b[j])) != 0;
        if (digit_a && digit_b) {
            // Compare whole digit runs as numbers: skip leading zeros, then the
            // longer run is larger; equal length compares digit by digit.
            size_t start_a = i;
            size_t start_b = j;
            while (start_a < a.size() && a[start_a] == '0') ++start_a;
            while (start_b < b.size() && b[start_b] == '0') ++start_b;
            size_t end_a = start_a;
            size_t end_b = start_b;
            while (end_a < a.size() && std::isdigit(static_cast<unsigned char>(a[end_a]))) ++end_a;
            while (end_b < b.size() && std::isdigit(static_cast<unsigned char>(b[end_b]))) ++end_b;
            const size_t length_a = end_a - start_a;
            const size_t length_b = end_b - start_b;
            if (length_a != length_b) {
                return length_a < length_b;
            }
            const int order = a.compare(start_a, length_a, b, start_b, length_b);
            if (order != 0) {
                return order < 0;
            }
            i = end_a;
            j = end_b;
        } else {
            if (a[i] != b[j]) {
                return a[i] < b[j];
            }
            ++i;
            ++j;
        }
    }
    return a.size() - i < b.size() - j;
}

ImageSequenceSource::ImageSequenceSource(std::string directory, Rational fps)
    : directory_(std::move(directory)), fps_(fps) {}

Status ImageSequenceSource::open() {
    files_.clear();
    stats_ = SourceStats{};
    next_index_ = 0;

    std::error_code error;
    if (!std::filesystem::is_directory(directory_, error)) {
        return Status(StatusCode::NotFound, "'" + directory_ + "' is not a directory");
    }
    for (const auto& entry : std::filesystem::directory_iterator(directory_, error)) {
        if (entry.is_regular_file() && is_image_file(entry.path())) {
            files_.push_back(entry.path().string());
        }
    }
    if (files_.empty()) {
        return Status(StatusCode::NotFound,
                      "no images (.jpg .png .bmp .tif .webp .ppm) found in '" + directory_ + "'");
    }
    std::sort(files_.begin(), files_.end(), natural_less);

    // The first image defines the resolution of the whole sequence.
    VideoFileSource first(files_.front());
    Status status = first.open();
    if (!status.ok()) {
        return status;
    }
    info_ = SourceInfo{};
    info_.uri = directory_;
    info_.container_name = "image sequence";
    info_.codec_name = first.info().codec_name;
    info_.native_pixel_format = first.info().native_pixel_format;
    info_.width = first.info().width;
    info_.height = first.info().height;
    info_.time_base = invert(fps_);  // one tick per image
    info_.nominal_fps = fps_;
    info_.frame_count_estimate = files_.size();
    info_.duration_ticks = static_cast<int64_t>(files_.size());
    info_.is_seekable = true;
    return Status::ok_status();
}

Status ImageSequenceSource::set_output_format(const FrameFormat& format) {
    if (format.width != info_.width || format.height != info_.height) {
        return Status(StatusCode::InvalidArgument, "output size must equal the image size");
    }
    output_format_ = format;
    format_set_ = true;
    return Status::ok_status();
}

Status ImageSequenceSource::seek(int64_t pts_ticks) {
    if (files_.empty()) {
        return Status(StatusCode::InvalidArgument, "seek() before open()");
    }
    // The time base is 1/fps, so tick i is image i.
    next_index_ = static_cast<size_t>(std::clamp<int64_t>(pts_ticks, 0, static_cast<int64_t>(files_.size()) - 1));
    return Status::ok_status();
}

ReadResult ImageSequenceSource::read_next(MutableFrameView destination, FrameTiming& timing) {
    if (!format_set_) {
        return ReadResult::error(Status(StatusCode::InvalidArgument, "set_output_format() not called"));
    }
    if (next_index_ >= files_.size()) {
        return ReadResult::end_of_stream();
    }
    const std::string& path = files_[next_index_];

    VideoFileSource image(path);
    Status status = image.open();
    if (status.ok() && (image.info().width != info_.width || image.info().height != info_.height)) {
        status = Status(StatusCode::Unsupported,
                        "image '" + path + "' is " + std::to_string(image.info().width) + "x" +
                            std::to_string(image.info().height) + " but the sequence is " +
                            std::to_string(info_.width) + "x" + std::to_string(info_.height) +
                            " (all images must have the same size)");
    }
    if (status.ok()) {
        status = image.set_output_format(output_format_);
    }
    if (!status.ok()) {
        return ReadResult::error(status);
    }
    FrameTiming ignored;
    ReadResult result = image.read_next(destination, ignored);
    if (!result.is_frame()) {
        return ReadResult::error(result.is_error() ? result.status()
                                                   : Status(StatusCode::InvalidData, "'" + path + "' has no image"));
    }
    timing.source_index = next_index_;
    timing.pts_ticks = static_cast<int64_t>(next_index_);
    timing.duration_ticks = 1;
    ++next_index_;
    ++stats_.frames_decoded;
    return ReadResult::frame();
}

}  // namespace vcam
