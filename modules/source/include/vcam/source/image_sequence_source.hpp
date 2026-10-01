// =============================================================================
// image_sequence_source.hpp — a directory of numbered images as a video
//
//   frames/000001.jpg, frames/000002.jpg, ...   (or .png, .bmp, .tif, .webp)
//
// Files are ordered with a NATURAL sort ("frame2.png" before "frame10.png"),
// and shown at a fixed rate (--source-fps, default 30): image i has
// timestamp i / fps. All images must have the same size (the camera's
// resolution equals the source resolution). Each image is decoded with the
// same FFmpeg code path as video files (VideoFileSource).
// =============================================================================
#pragma once

#include <string>
#include <vector>

#include "vcam/source/frame_source.hpp"

namespace vcam {

class ImageSequenceSource final : public FrameSource {
public:
    ImageSequenceSource(std::string directory, Rational fps);

    Status open() override;
    const SourceInfo& info() const override { return info_; }
    Status set_output_format(const FrameFormat& format) override;
    ReadResult read_next(MutableFrameView destination, FrameTiming& timing) override;
    Status seek(int64_t pts_ticks) override;  // one tick per image
    SourceStats stats() const override { return stats_; }
    void close() override {}

    const std::vector<std::string>& files() const { return files_; }

private:
    std::string directory_;
    Rational fps_;
    SourceInfo info_;
    SourceStats stats_;
    std::vector<std::string> files_;
    FrameFormat output_format_{};
    bool format_set_ = false;
    size_t next_index_ = 0;
};

// "frame2" < "frame10": digit runs compare as numbers.
bool natural_less(const std::string& a, const std::string& b);

}  // namespace vcam
