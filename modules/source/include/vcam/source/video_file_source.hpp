// =============================================================================
// video_file_source.hpp — FrameSource for video files (.mp4 .mkv .avi .mov .webm …)
//
// Uses FFmpeg's libavformat (container reading, "demuxing") and libavcodec
// (decoding). Only the first/best video stream is used; audio and subtitle
// streams are ignored.
//
// Timestamps: every frame's presentation time comes from FFmpeg's
// `best_effort_timestamp` in the stream's own time base, shifted so the first
// frame is 0. Missing or non-increasing timestamps are repaired (and counted),
// never silently invented beyond that. Variable-frame-rate files keep their
// real timestamps.
//
// Errors:
//   * open(): missing file, unreadable file, not a media file, no video
//     stream, no decoder → descriptive Status, nothing decoded.
//   * read_next(): a corrupted packet is skipped and counted (stats()), the
//     file continues; only unrecoverable problems return ReadResult::error.
// =============================================================================
#pragma once

#include <memory>
#include <optional>
#include <string>

#include "vcam/core/log.hpp"
#include "vcam/source/frame_source.hpp"

// FFmpeg types (forward declarations: this header needs no FFmpeg headers).
struct AVCodecContext;
struct AVFormatContext;
struct AVFrame;
struct AVPacket;

namespace vcam {

class FormatConverter;  // from the convert module (linked privately)

class VideoFileSource final : public FrameSource {
public:
    explicit VideoFileSource(std::string path);
    ~VideoFileSource() override;

    VideoFileSource(const VideoFileSource&) = delete;
    VideoFileSource& operator=(const VideoFileSource&) = delete;

    Status open() override;
    const SourceInfo& info() const override { return info_; }
    Status set_output_format(const FrameFormat& format) override;
    ReadResult read_next(MutableFrameView destination, FrameTiming& timing) override;

    // Jumps to source time `pts_ticks` (normalised ticks, first frame = 0). The
    // next read_next() returns the frame on screen at that time. Works after
    // end of stream too (used to loop). Implementation: seek to the keyframe
    // before the target, then decode and DISCARD frames that end before it
    // (they are never converted, so this is cheap).
    Status seek(int64_t pts_ticks) override;

    // Decoder threads: 0 = one per CPU core (fastest loading), small values
    // use less memory and CPU when decoding in real time. Call before open().
    void set_decoder_threads(int threads) { decoder_threads_ = threads; }

    SourceStats stats() const override { return stats_; }
    void close() override;

private:
    // Reads packets until one is accepted by the decoder (or the file ends,
    // in which case the decoder is switched to "drain" mode).
    Status feed_decoder();

    // Fills `timing` for the frame in frame_ (normalises and repairs timestamps).
    void compute_timing(FrameTiming& timing);

    void fill_info_from_stream();

    // Custom deleters: each FFmpeg object has its own free function, and
    // std::unique_ptr calls it automatically (RAII: no leaks on any return path).
    struct FormatContextDeleter { void operator()(AVFormatContext* context) const; };
    struct CodecContextDeleter  { void operator()(AVCodecContext* context) const; };
    struct PacketDeleter        { void operator()(AVPacket* packet) const; };
    struct FrameDeleter         { void operator()(AVFrame* frame) const; };

    std::string path_;
    SourceInfo info_;
    SourceStats stats_;

    std::unique_ptr<AVFormatContext, FormatContextDeleter> format_context_;
    std::unique_ptr<AVCodecContext, CodecContextDeleter> codec_context_;
    std::unique_ptr<AVPacket, PacketDeleter> packet_;
    std::unique_ptr<AVFrame, FrameDeleter> frame_;
    std::unique_ptr<FormatConverter> converter_;

    int stream_index_ = -1;
    bool draining_ = false;  // end of file reached, decoder is emptying its queue
    bool finished_ = false;  // decoder fully drained: no more frames

    // Timestamp normalisation state.
    std::optional<int64_t> first_raw_pts_;
    std::optional<int64_t> last_pts_;
    int64_t nominal_frame_ticks_ = 1;  // one frame period in time-base ticks
    uint64_t next_source_index_ = 0;
    // Seek state: after a seek, frames are decoded but dropped until the frame
    // on screen at the target time is reached.
    Status start_seek(int64_t target_ticks, int64_t margin_ticks);
    Status reopen();  // close + open, keeping the output format and statistics
    std::optional<int64_t> discard_before_;  // target time of the current seek
    int64_t seek_margin_ticks_ = 0;          // how far before the target we jumped
    int64_t seek_position_ticks_ = 0;        // where we asked the demuxer to land
    bool seek_found_key_ = false;            // a keyframe was decoded since the seek
    int decoder_threads_ = 0;

    // Repeated warnings are printed at most once per second.
    RateLimiter decode_error_warnings_{1'000'000'000};
    RateLimiter corrupt_frame_warnings_{1'000'000'000};
    RateLimiter timestamp_warnings_{1'000'000'000};
};

}  // namespace vcam
