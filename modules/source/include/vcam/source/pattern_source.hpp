// =============================================================================
// pattern_source.hpp — synthetic test video (no file needed)
//
// Each frame shows colour bars, a box that moves one step per frame (so
// motion judder is visible) and a barcode with the frame number
// (frame_barcode.hpp). Used for testing the whole pipeline and real
// applications: `virtual-camera --input pattern`.
// =============================================================================
#pragma once

#include <memory>

#include "vcam/source/frame_source.hpp"

namespace vcam {

class FormatConverter;

struct PatternOptions {
    int width = 1280;
    int height = 720;
    Rational fps{30, 1};
    uint64_t frame_count = 300;  // 10 s at 30 fps
};

class PatternSource final : public FrameSource {
public:
    explicit PatternSource(PatternOptions options);
    ~PatternSource() override;

    Status open() override;
    const SourceInfo& info() const override { return info_; }
    Status set_output_format(const FrameFormat& format) override;
    ReadResult read_next(MutableFrameView destination, FrameTiming& timing) override;
    Status seek(int64_t pts_ticks) override;  // one tick per frame
    SourceStats stats() const override { return stats_; }
    void close() override;

private:
    PatternOptions options_;
    SourceInfo info_;
    SourceStats stats_;
    OwnedFrame background_;  // colour bars, drawn once
    OwnedFrame canvas_;      // background + moving box + barcode, per frame
    std::unique_ptr<FormatConverter> converter_;
    uint64_t next_index_ = 0;
};

}  // namespace vcam
