// =============================================================================
// frame_source.hpp — the abstraction every input source implements
//
// A FrameSource answers ONE question: "how are frames obtained?"
// It knows nothing about /dev/videoX, output timing or buffering.
//
// Typical use (this is what tools/probe_source.cpp and, later, the loader
// thread do):
//
//     VideoFileSource source("clip.mp4");
//     source.open();                                         // probe metadata
//     auto format = make_output_format(source.info(), PixelFormat::YUYV);
//     source.set_output_format(format.value());
//     OwnedFrame slot(format.value());                       // or a buffer slot
//     FrameTiming timing;
//     while (source.read_next(slot.mutable_view(), timing).is_frame()) { ... }
//     source.close();
//
// Implementations: VideoFileSource, ImageSequenceSource, PatternSource
// (test pattern with frame-number barcode) and PushSource (script input).
// =============================================================================
#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "vcam/core/frame.hpp"
#include "vcam/core/rational.hpp"
#include "vcam/core/status.hpp"

namespace vcam {

// Metadata known after open(). Anything the container may not report is optional.
struct SourceInfo {
    std::string uri;                     // file path (or other identifier)
    std::string container_name;          // e.g. "mov,mp4,m4a,3gp,3g2,mj2"
    std::string codec_name;              // e.g. "h264"
    std::string native_pixel_format;     // e.g. "yuv420p"
    int width = 0;                       // native resolution == output resolution
    int height = 0;

    Rational time_base{0, 1};            // unit of FrameTiming::pts_ticks, e.g. 1/15360 s
    std::optional<Rational> nominal_fps; // average frame rate declared by the file
    std::optional<uint64_t> frame_count_estimate;  // container's frame count (may be inexact)
    std::optional<int64_t> duration_ticks;         // stream duration in time_base ticks

    bool variable_frame_rate_suspected = false;    // container hints at VFR (confirmed while decoding)
    bool is_live = false;                // live sources (scripts, network) have no end and no seek
    bool is_seekable = false;
};

// Counters a source keeps while decoding.
struct SourceStats {
    uint64_t frames_decoded = 0;     // frames successfully delivered by read_next()
    uint64_t decode_errors = 0;      // packets the decoder rejected (skipped)
    uint64_t corrupt_frames = 0;     // frames delivered but flagged as damaged by the decoder
    uint64_t timestamp_repairs = 0;  // missing or non-increasing timestamps that were fixed
};

// Outcome of read_next().
class ReadResult {
public:
    // Again: no frame available right now (live sources); call again later.
    enum class Kind { Frame, EndOfStream, Again, Error };

    static ReadResult frame() { return ReadResult(Kind::Frame, Status::ok_status()); }
    static ReadResult end_of_stream() { return ReadResult(Kind::EndOfStream, Status::ok_status()); }
    static ReadResult again() { return ReadResult(Kind::Again, Status::ok_status()); }
    static ReadResult error(Status status) { return ReadResult(Kind::Error, std::move(status)); }

    Kind kind() const { return kind_; }
    bool is_frame() const { return kind_ == Kind::Frame; }
    bool is_end_of_stream() const { return kind_ == Kind::EndOfStream; }
    bool is_again() const { return kind_ == Kind::Again; }
    bool is_error() const { return kind_ == Kind::Error; }
    const Status& status() const { return status_; }

private:
    ReadResult(Kind kind, Status status) : kind_(kind), status_(std::move(status)) {}
    Kind kind_;
    Status status_;
};

class FrameSource {
public:
    virtual ~FrameSource() = default;

    // Opens and validates the source and fills info(). No frames are decoded yet.
    virtual Status open() = 0;

    // Valid after a successful open().
    virtual const SourceInfo& info() const = 0;

    // Chooses the pixel format frames are delivered in. Width/height must equal
    // info().width/height (no scaling by design). Must be called before read_next().
    virtual Status set_output_format(const FrameFormat& format) = 0;

    // Decodes the next frame (presentation order) into `destination` and fills
    // `timing`. Returns Frame, EndOfStream, or Error (a fatal problem; recoverable
    // problems such as one corrupted packet are counted in stats() instead).
    virtual ReadResult read_next(MutableFrameView destination, FrameTiming& timing) = 0;

    // Moves the read position. Default: not supported (Phase 7 adds it).
    virtual Status seek(int64_t /*pts_ticks*/) {
        return Status(StatusCode::Unsupported, "seeking is not supported by this source yet");
    }

    virtual SourceStats stats() const = 0;

    // Asks a blocking open()/read_next() in another thread to return soon
    // (used for shutdown). Default: nothing to interrupt.
    virtual void interrupt() {}

    // Releases decoder resources. Safe to call more than once.
    virtual void close() = 0;
};

// The one place where the rule "output resolution = source resolution" lives.
// YUV outputs are signalled as BT.601 limited range (the webcam convention);
// the converter maps the source's colours to that.
Result<FrameFormat> make_output_format(const SourceInfo& info, PixelFormat pixel_format);

}  // namespace vcam
