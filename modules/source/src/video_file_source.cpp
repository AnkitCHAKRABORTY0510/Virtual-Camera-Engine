#include "vcam/source/video_file_source.hpp"

extern "C" {
#include <libavutil/pixdesc.h>  // av_get_pix_fmt_name
}

#include <algorithm>

#include "ffmpeg_util.hpp"
#include "vcam/convert/format_converter.hpp"

namespace vcam {

namespace {

constexpr const char* kModule = "source";

// If the decoder reports this many errors in a row without producing a single
// frame, the file is considered unreadable rather than "a few bad packets".
constexpr int kMaxConsecutiveDecodeErrors = 200;

bool valid_rate(AVRational rate) {
    return rate.num > 0 && rate.den > 0;
}

Rational to_rational(AVRational rate) {
    return make_rational(rate.num, rate.den);
}

}  // namespace

// ---- RAII deleters -----------------------------------------------------------

void VideoFileSource::FormatContextDeleter::operator()(AVFormatContext* context) const {
    // avformat_close_input closes the file and frees the context.
    avformat_close_input(&context);
}

void VideoFileSource::CodecContextDeleter::operator()(AVCodecContext* context) const {
    avcodec_free_context(&context);
}

void VideoFileSource::PacketDeleter::operator()(AVPacket* packet) const {
    av_packet_free(&packet);
}

void VideoFileSource::FrameDeleter::operator()(AVFrame* frame) const {
    av_frame_free(&frame);
}

// ---- Construction ------------------------------------------------------------

VideoFileSource::VideoFileSource(std::string path) : path_(std::move(path)) {
    info_.uri = path_;
}

// Defined here (not in the header) because FormatConverter is an incomplete
// type in the header; unique_ptr needs the full type to delete it.
VideoFileSource::~VideoFileSource() {
    close();
}

// ---- open() ------------------------------------------------------------------

Status VideoFileSource::open() {
    close();  // allow re-opening
    stats_ = SourceStats{};
    info_ = SourceInfo{};
    info_.uri = path_;
    ffmpeg::configure_logging();

    // 1. Open the container. avformat_open_input detects the format (mp4, mkv, …)
    //    from the file content and reads the header.
    AVFormatContext* raw_format_context = nullptr;
    int result = avformat_open_input(&raw_format_context, path_.c_str(), nullptr, nullptr);
    if (result < 0) {
        return ffmpeg::to_status(result, "cannot open '" + path_ + "'");
    }
    format_context_.reset(raw_format_context);

    // 2. Read a little of the file to learn stream parameters that the header
    //    may not contain (frame rate, pixel format, …).
    result = avformat_find_stream_info(format_context_.get(), nullptr);
    if (result < 0) {
        Status status = ffmpeg::to_status(result, "cannot read stream information from '" + path_ + "'");
        close();
        return status;
    }

    // 3. Pick the "best" video stream (FFmpeg's own heuristic: resolution, bitrate, …).
    result = av_find_best_stream(format_context_.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (result < 0 ||
        (format_context_->streams[result]->disposition & AV_DISPOSITION_ATTACHED_PIC) != 0) {
        // An "attached picture" is cover art in an audio file, not video.
        close();
        return Status(StatusCode::Unsupported, "'" + path_ + "' contains no video stream");
    }
    stream_index_ = result;
    AVStream* stream = format_context_->streams[stream_index_];

    // Tell the demuxer to drop packets of all other streams early (less work).
    for (unsigned i = 0; i < format_context_->nb_streams; ++i) {
        if (static_cast<int>(i) != stream_index_) {
            format_context_->streams[i]->discard = AVDISCARD_ALL;
        }
    }

    // 4. Create and open the decoder for this stream's codec.
    const AVCodec* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
    if (decoder == nullptr) {
        std::string codec = avcodec_get_name(stream->codecpar->codec_id);
        close();
        return Status(StatusCode::Unsupported, "no decoder available for codec '" + codec + "' in '" + path_ + "'");
    }

    codec_context_.reset(avcodec_alloc_context3(decoder));
    if (!codec_context_) {
        close();
        return Status(StatusCode::ResourceExhausted, "cannot allocate decoder context");
    }
    // Copy the stream's codec parameters (size, pixel format, extradata) into the decoder.
    result = avcodec_parameters_to_context(codec_context_.get(), stream->codecpar);
    if (result < 0) {
        Status status = ffmpeg::to_status(result, "cannot configure decoder");
        close();
        return status;
    }
    codec_context_->pkt_timebase = stream->time_base;  // lets the decoder compute timestamps correctly
    // 0 = as many threads as CPU cores (fast bulk decoding); a small number
    // keeps memory and CPU low when decoding in real time (stream mode).
    codec_context_->thread_count = decoder_threads_;

    result = avcodec_open2(codec_context_.get(), decoder, nullptr);
    if (result < 0) {
        Status status = ffmpeg::to_status(result, "cannot open decoder '" + std::string(decoder->name) + "'");
        close();
        return status;
    }

    packet_.reset(av_packet_alloc());
    frame_.reset(av_frame_alloc());
    if (!packet_ || !frame_) {
        close();
        return Status(StatusCode::ResourceExhausted, "cannot allocate packet/frame");
    }

    fill_info_from_stream();
    if (info_.width <= 0 || info_.height <= 0) {
        Status status(StatusCode::InvalidData, "'" + path_ + "' reports an invalid frame size");
        close();
        return status;
    }

    VCAM_DEBUG(kModule, "opened '" << path_ << "': " << info_.codec_name << " " << info_.width << "x" << info_.height
                                  << " " << info_.native_pixel_format << ", fps "
                                  << (info_.nominal_fps ? to_string(*info_.nominal_fps) : std::string("unknown"))
                                  << ", time base " << to_string(info_.time_base));
    return Status::ok_status();
}

void VideoFileSource::fill_info_from_stream() {
    AVStream* stream = format_context_->streams[stream_index_];
    const AVCodecParameters* parameters = stream->codecpar;

    info_.container_name = format_context_->iformat ? format_context_->iformat->name : "unknown";
    info_.codec_name = avcodec_get_name(parameters->codec_id);
    const char* pixel_format_name = av_get_pix_fmt_name(static_cast<AVPixelFormat>(parameters->format));
    info_.native_pixel_format = pixel_format_name ? pixel_format_name : "unknown";
    info_.width = parameters->width;
    info_.height = parameters->height;
    info_.time_base = to_rational(stream->time_base);

    // avg_frame_rate = total frames / duration (what the file "plays at");
    // r_frame_rate   = FFmpeg's guess of the base rate. They differ for VFR files.
    if (valid_rate(stream->avg_frame_rate)) {
        info_.nominal_fps = to_rational(stream->avg_frame_rate);
    } else if (valid_rate(stream->r_frame_rate)) {
        info_.nominal_fps = to_rational(stream->r_frame_rate);
    }
    info_.variable_frame_rate_suspected = valid_rate(stream->avg_frame_rate) && valid_rate(stream->r_frame_rate) &&
                                          av_cmp_q(stream->avg_frame_rate, stream->r_frame_rate) != 0;

    if (stream->nb_frames > 0) {
        info_.frame_count_estimate = static_cast<uint64_t>(stream->nb_frames);
    }
    if (stream->duration != AV_NOPTS_VALUE && stream->duration > 0) {
        info_.duration_ticks = stream->duration;
    } else if (format_context_->duration != AV_NOPTS_VALUE && format_context_->duration > 0) {
        // Container duration is in AV_TIME_BASE (microsecond) units.
        info_.duration_ticks = rescale(format_context_->duration, make_rational(1, AV_TIME_BASE), info_.time_base);
    }
    if (!info_.frame_count_estimate && info_.duration_ticks && info_.nominal_fps) {
        // Estimate = duration [s] * fps
        const Rational one_frame = invert(*info_.nominal_fps);
        info_.frame_count_estimate =
            static_cast<uint64_t>(rescale(*info_.duration_ticks, info_.time_base, one_frame, Rounding::Nearest));
    }

    // One frame period in ticks, used to repair missing timestamps.
    if (info_.nominal_fps && is_positive(info_.time_base)) {
        nominal_frame_ticks_ = std::max<int64_t>(1, rescale(1, invert(*info_.nominal_fps), info_.time_base));
    } else {
        nominal_frame_ticks_ = 1;
    }

    info_.is_live = false;
    info_.is_seekable = true;
}

// ---- set_output_format() -------------------------------------------------------

Status VideoFileSource::set_output_format(const FrameFormat& format) {
    if (!codec_context_) {
        return Status(StatusCode::InvalidArgument, "set_output_format() called before a successful open()");
    }
    if (format.width != info_.width || format.height != info_.height) {
        return Status(StatusCode::InvalidArgument,
                      "output size must equal the source size " + std::to_string(info_.width) + "x" +
                          std::to_string(info_.height) + " (no scaling)");
    }
    converter_ = std::make_unique<FormatConverter>(format);
    VCAM_DEBUG(kModule, "output format " << describe(format));
    return Status::ok_status();
}

// ---- read_next() -----------------------------------------------------------------

ReadResult VideoFileSource::read_next(MutableFrameView destination, FrameTiming& timing) {
    if (!converter_) {
        return ReadResult::error(Status(StatusCode::InvalidArgument, "read_next() called before set_output_format()"));
    }
    if (finished_) {
        return ReadResult::end_of_stream();
    }

    int consecutive_errors = 0;
    while (true) {
        // FFmpeg's decode API is a two-way queue:
        //   avcodec_send_packet()   pushes compressed data in,
        //   avcodec_receive_frame() pulls decoded frames out.
        // receive returns EAGAIN when it needs more input, and AVERROR_EOF
        // once it has been drained after the end of the file.
        const int result = avcodec_receive_frame(codec_context_.get(), frame_.get());

        if (result == 0) {
            compute_timing(timing);

            // After a seek: skip frames until the one on screen at the target
            // time, without converting them (cheap).
            if (discard_before_) {
                if (!seek_found_key_) {
                    // Frames before the first keyframe reference pictures we
                    // never decoded: they would be damaged. Skip them.
                    if (!ffmpeg::is_keyframe(*frame_)) {
                        av_frame_unref(frame_.get());
                        continue;
                    }
                    seek_found_key_ = true;
                    // Demuxers without a keyframe index (e.g. MPEG-TS) can land
                    // AFTER the target. Then jump further back and try again
                    // (1 s, 2 s, 4 s ... before the target).
                    const int64_t one_second = rescale(1, make_rational(1, 1), info_.time_base);
                    if (timing.pts_ticks > *discard_before_ && seek_position_ticks_ > 0 &&
                        seek_margin_ticks_ < 16 * one_second) {
                        const int64_t margin = seek_margin_ticks_ == 0 ? one_second : 2 * seek_margin_ticks_;
                        av_frame_unref(frame_.get());
                        Status retried = start_seek(*discard_before_, margin);
                        if (!retried.ok()) {
                            return ReadResult::error(retried);
                        }
                        continue;
                    }
                }
                if (timing.pts_ticks + timing.duration_ticks <= *discard_before_) {
                    av_frame_unref(frame_.get());
                    continue;
                }
                discard_before_.reset();
            }

            const bool corrupt = (frame_->flags & AV_FRAME_FLAG_CORRUPT) != 0 || frame_->decode_error_flags != 0;
            if (corrupt) {
                ++stats_.corrupt_frames;
                if (corrupt_frame_warnings_.allow()) {
                    VCAM_WARN(kModule, "frame " << timing.source_index << " is damaged (decoder concealed errors)"
                                                << " [" << corrupt_frame_warnings_.take_suppressed()
                                                << " similar suppressed]");
                }
            }

            Status converted = converter_->convert(*frame_, destination);
            av_frame_unref(frame_.get());  // release the decoder's buffer for reuse
            if (!converted.ok()) {
                return ReadResult::error(converted);
            }
            ++stats_.frames_decoded;
            return ReadResult::frame();
        }

        if (result == AVERROR_EOF) {
            finished_ = true;
            VCAM_DEBUG(kModule, "decoder drained after " << stats_.frames_decoded << " frames");
            return ReadResult::end_of_stream();
        }

        if (result != AVERROR(EAGAIN)) {
            // Some decoders (with frame threading) report a bad frame here.
            ++stats_.decode_errors;
            if (++consecutive_errors > kMaxConsecutiveDecodeErrors) {
                return ReadResult::error(ffmpeg::to_status(result, "decoder keeps failing on '" + path_ + "'"));
            }
            if (decode_error_warnings_.allow()) {
                VCAM_WARN(kModule, "decode error: " << ffmpeg::error_string(result));
            }
            continue;
        }

        // The decoder needs more input.
        if (draining_) {
            // Already told the decoder the file ended; it should return EOF, not EAGAIN.
            finished_ = true;
            return ReadResult::end_of_stream();
        }
        Status fed = feed_decoder();
        if (!fed.ok()) {
            return ReadResult::error(fed);
        }
    }
}

Status VideoFileSource::feed_decoder() {
    int consecutive_errors = 0;
    while (true) {
        // av_read_frame returns the next compressed packet of ANY stream.
        const int read_result = av_read_frame(format_context_.get(), packet_.get());
        if (read_result < 0) {
            if (read_result != AVERROR_EOF) {
                // Truncated or damaged file: decode what we have, then stop.
                ++stats_.decode_errors;
                VCAM_WARN(kModule, "read error in '" << path_ << "': " << ffmpeg::error_string(read_result)
                                                     << " — treating as end of file");
            }
            // Sending a null packet switches the decoder to drain mode: it
            // returns the frames it still holds, then AVERROR_EOF.
            avcodec_send_packet(codec_context_.get(), nullptr);
            draining_ = true;
            return Status::ok_status();
        }

        if (packet_->stream_index != stream_index_) {
            av_packet_unref(packet_.get());  // not our stream: drop it
            continue;
        }

        const int send_result = avcodec_send_packet(codec_context_.get(), packet_.get());
        av_packet_unref(packet_.get());
        if (send_result == 0 || send_result == AVERROR(EAGAIN)) {
            return Status::ok_status();
        }

        // A corrupted packet: skip it and keep going with the next one.
        ++stats_.decode_errors;
        if (++consecutive_errors > kMaxConsecutiveDecodeErrors) {
            return ffmpeg::to_status(send_result, "too many consecutive decode errors in '" + path_ + "'");
        }
        if (decode_error_warnings_.allow()) {
            VCAM_WARN(kModule, "skipping undecodable packet: " << ffmpeg::error_string(send_result) << " ["
                                                               << decode_error_warnings_.take_suppressed()
                                                               << " similar suppressed]");
        }
    }
}

void VideoFileSource::compute_timing(FrameTiming& timing) {
    // best_effort_timestamp is FFmpeg's most reliable presentation time for
    // the frame (it falls back to packet timestamps when the frame has none).
    const int64_t raw_pts = frame_->best_effort_timestamp;
    int64_t pts = 0;

    if (raw_pts == AV_NOPTS_VALUE) {
        // No timestamp at all: continue the sequence at the nominal frame rate.
        pts = last_pts_ ? *last_pts_ + nominal_frame_ticks_ : 0;
        ++stats_.timestamp_repairs;
        if (timestamp_warnings_.allow()) {
            VCAM_WARN(kModule, "frame without timestamp; using nominal frame rate");
        }
    } else {
        if (!first_raw_pts_) {
            first_raw_pts_ = raw_pts;  // the first frame defines time 0
        }
        pts = raw_pts - *first_raw_pts_;
    }

    if (last_pts_ && pts <= *last_pts_) {
        // Timestamps must strictly increase in presentation order.
        const int64_t repaired = *last_pts_ + nominal_frame_ticks_;
        ++stats_.timestamp_repairs;
        if (timestamp_warnings_.allow()) {
            VCAM_WARN(kModule, "non-increasing timestamp " << pts << " after " << *last_pts_ << ", using "
                                                           << repaired);
        }
        pts = repaired;
    }

    const int64_t duration = ffmpeg::frame_duration(*frame_);

    timing.source_index = next_source_index_++;
    timing.pts_ticks = pts;
    timing.duration_ticks = duration > 0 ? duration : nominal_frame_ticks_;
    last_pts_ = pts;
}

// ---- seek() ----------------------------------------------------------------------

Status VideoFileSource::seek(int64_t pts_ticks) {
    if (!format_context_ || !codec_context_) {
        return Status(StatusCode::InvalidArgument, "seek() before open()");
    }
    AVStream* stream = format_context_->streams[stream_index_];
    if (!first_raw_pts_) {
        // Nothing decoded yet: the stream's start time defines time 0.
        first_raw_pts_ = stream->start_time != AV_NOPTS_VALUE ? stream->start_time : 0;
    }
    return start_seek(std::max<int64_t>(pts_ticks, 0), 0);
}

Status VideoFileSource::start_seek(int64_t target_ticks, int64_t margin_ticks) {
    seek_position_ticks_ = std::max<int64_t>(target_ticks - margin_ticks, 0);
    if (seek_position_ticks_ == 0) {
        // Back to the very start (also how looping works): reopening the file
        // is exact for every container (timestamp seeks to the start are not
        // reliable for MPEG-TS, byte seeks not for Matroska) and takes only a
        // few milliseconds.
        Status reopened = reopen();
        if (!reopened.ok()) {
            return reopened;
        }
    } else {
        // av_seek_frame with AVSEEK_FLAG_BACKWARD asks for the last keyframe at
        // or before the position (decoding must start at a keyframe).
        const int result = av_seek_frame(format_context_.get(), stream_index_,
                                         seek_position_ticks_ + *first_raw_pts_, AVSEEK_FLAG_BACKWARD);
        if (result < 0) {
            return ffmpeg::to_status(result, "cannot seek in '" + path_ + "'");
        }
        // Forget frames still inside the decoder; also re-arms it after end of stream.
        avcodec_flush_buffers(codec_context_.get());
        draining_ = false;
        finished_ = false;
    }
    last_pts_.reset();
    discard_before_ = target_ticks;
    seek_margin_ticks_ = margin_ticks;
    seek_found_key_ = false;
    next_source_index_ =
        static_cast<uint64_t>(nominal_frame_ticks_ > 0 ? target_ticks / nominal_frame_ticks_ : 0);
    return Status::ok_status();
}

Status VideoFileSource::reopen() {
    // Keep what must survive: the output format and the cumulative statistics.
    const std::optional<FrameFormat> output =
        converter_ ? std::optional<FrameFormat>(converter_->output_format()) : std::nullopt;
    const SourceStats saved_stats = stats_;
    Status status = open();
    if (status.ok() && output) {
        status = set_output_format(*output);
    }
    stats_ = saved_stats;
    return status;
}

// ---- close() ---------------------------------------------------------------------

void VideoFileSource::close() {
    // Order: converter and frames first, then the decoder, then the file.
    converter_.reset();
    frame_.reset();
    packet_.reset();
    codec_context_.reset();
    format_context_.reset();

    stream_index_ = -1;
    draining_ = false;
    finished_ = false;
    first_raw_pts_.reset();
    last_pts_.reset();
    discard_before_.reset();
    next_source_index_ = 0;
}

}  // namespace vcam
