// =============================================================================
// ffmpeg_util.hpp — PRIVATE helpers for the source module (not installed,
// not visible to other modules).
//
// Collects the few places where FFmpeg versions differ, so the rest of the
// code is version-independent. Supported: FFmpeg 4.4 (Ubuntu 22.04),
// 6.x (Ubuntu 24.04) and 7.x.
// =============================================================================
#pragma once

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
}

#include <string>

#include "vcam/core/log.hpp"
#include "vcam/core/status.hpp"

namespace vcam::ffmpeg {

// Human-readable text for an FFmpeg error code (negative AVERROR value).
inline std::string error_string(int error_code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {0};
    // av_strerror fills the buffer with a description such as
    // "No such file or directory" or "Invalid data found when processing input".
    av_strerror(error_code, buffer, sizeof(buffer));
    return buffer;
}

// Maps an FFmpeg error code to our StatusCode, keeping FFmpeg's text.
inline Status to_status(int error_code, const std::string& context) {
    StatusCode code = StatusCode::IoError;
    if (error_code == AVERROR(ENOENT)) {
        code = StatusCode::NotFound;
    } else if (error_code == AVERROR(EACCES) || error_code == AVERROR(EPERM)) {
        code = StatusCode::PermissionDenied;
    } else if (error_code == AVERROR_INVALIDDATA) {
        code = StatusCode::InvalidData;
    } else if (error_code == AVERROR_DECODER_NOT_FOUND || error_code == AVERROR_DEMUXER_NOT_FOUND ||
               error_code == AVERROR_STREAM_NOT_FOUND || error_code == AVERROR_PATCHWELCOME) {
        code = StatusCode::Unsupported;
    } else if (error_code == AVERROR(ENOMEM)) {
        code = StatusCode::ResourceExhausted;
    }
    return Status(code, context + ": " + error_string(error_code));
}

// Display duration of a decoded frame in stream time-base ticks (0 = unknown).
// FFmpeg 6.0 (libavutil 58.2.100) renamed AVFrame::pkt_duration to ::duration.
inline int64_t frame_duration(const AVFrame& frame) {
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(58, 2, 100)
    return frame.duration;
#else
    return frame.pkt_duration;
#endif
}

// True for a keyframe (decoding can start here). FFmpeg 6.1 (libavutil
// 58.29.100) replaced AVFrame::key_frame with the AV_FRAME_FLAG_KEY flag.
inline bool is_keyframe(const AVFrame& frame) {
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(58, 29, 100)
    return (frame.flags & AV_FRAME_FLAG_KEY) != 0;
#else
    return frame.key_frame != 0;
#endif
}

// FFmpeg prints its own messages to stderr. We report problems ourselves
// (counted and rate-limited), so FFmpeg's output is only shown at our
// DEBUG/TRACE levels.
inline void configure_logging() {
    if (log::enabled(LogLevel::Trace)) {
        av_log_set_level(AV_LOG_VERBOSE);
    } else if (log::enabled(LogLevel::Debug)) {
        av_log_set_level(AV_LOG_WARNING);
    } else {
        av_log_set_level(AV_LOG_FATAL);
    }
}

}  // namespace vcam::ffmpeg
