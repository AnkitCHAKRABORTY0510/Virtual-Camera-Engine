#include "vcam/control/engine.hpp"

#include <sys/eventfd.h>  // eventfd: a counter file descriptor used as a wake-up signal
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

#include "vcam/buffer/live_frame_buffer.hpp"
#include "vcam/buffer/memory_budget.hpp"
#include "vcam/buffer/slab_buffer.hpp"
#include "vcam/core/clock_time.hpp"
#include "vcam/core/log.hpp"
#include "vcam/core/text_format.hpp"
#include "vcam/output/v4l2_loopback_camera.hpp"
#include "vcam/resample/playhead.hpp"
#include "vcam/resample/source_clock.hpp"
#include "vcam/source/source_factory.hpp"
#include "vcam/timing/clock.hpp"
#include "vcam/timing/realtime.hpp"
#include "vcam/timing/scheduler.hpp"

namespace vcam {

namespace {

constexpr const char* kModule = "engine";

void notify(int fd) {
    const uint64_t one = 1;
    // Writing to an eventfd adds to its counter and makes it readable for poll().
    [[maybe_unused]] const ssize_t ignored = ::write(fd, &one, sizeof(one));
}

std::string buffer_advice() {
    return "Try --buffer-mode disk (temporary file), --pixel-format i420 (25 % smaller), "
           "a larger --memory-limit, or a shorter clip.";
}

}  // namespace

// Size of the stream-mode window, in frames:
//   wanted = read_ahead seconds × source frame rate (+2: the frame on screen
//            and the one being decoded)
// capped so the window never takes more than `memory_limit` bytes (or 256 MiB
// when no limit is set — a 4K video at 60 fps would otherwise need ~1 GB per
// second of read-ahead). At least 4 frames.
size_t Engine::stream_capacity(const SourceInfo& info) const {
    Rational rate = output_fps_;
    if (info.nominal_fps && is_positive(*info.nominal_fps)) {
        rate = *info.nominal_fps;
    }
    const double wanted = std::ceil(config_.buffer.read_ahead_s * to_double(rate)) + 2;
    const uint64_t frame_bytes = std::max<uint64_t>(estimate_buffer_bytes(output_format_, 1), 1);
    const uint64_t limit = config_.buffer.memory_limit ? config_.buffer.memory_limit : (256ull << 20);
    const uint64_t affordable = limit / frame_bytes;
    size_t capacity = static_cast<size_t>(std::max(wanted, 4.0));
    if (capacity > affordable) {
        capacity = static_cast<size_t>(std::max<uint64_t>(affordable, 4));
        VCAM_INFO(kModule, "read-ahead limited to " << capacity << " frames (" << format_bytes(capacity * frame_bytes)
                                                    << ") by the memory limit");
    }
    return capacity;
}

// ============================================================================
// Construction / lifecycle
// ============================================================================

Engine::Engine(Config config) : config_(std::move(config)) {
    event_fd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    state_.set_listener([this](EngineState from, EngineState to) {
        VCAM_DEBUG(kModule, "state " << state_name(from) << " -> " << state_name(to));
        notify(event_fd_);
    });
}

Engine::~Engine() {
    request_stop();
    shutdown();
    if (event_fd_ >= 0) {
        ::close(event_fd_);
    }
}

void Engine::clear_event() {
    uint64_t value = 0;
    [[maybe_unused]] const ssize_t ignored = ::read(event_fd_, &value, sizeof(value));
}

Status Engine::start() {
    Status status = validate(config_);
    if (!status.ok()) {
        fail(status, kExitConfig);
        return status;
    }
    state_.transition(EngineState::Loading);
    loader_ = std::thread(&Engine::loader_main, this);
    return Status::ok_status();
}

void Engine::request_stop() {
    if (stop_.exchange(true)) {
        return;  // already stopping
    }
    state_.transition(EngineState::Stopping);  // ignored if already in ERROR
    {
        std::lock_guard<std::mutex> lock(source_mutex_);
        if (source_) {
            source_->interrupt();  // wake a source blocked waiting for a producer
        }
    }
    notify(event_fd_);
}

void Engine::shutdown() {
    request_stop();
    // Each thread checks stop_ at least once per frame, so joining is quick:
    // at most one output period (pacer) or one decoded frame (loader).
    if (pacer_.joinable()) {
        pacer_.join();
    }
    if (loader_.joinable()) {
        loader_.join();
    }
    if (camera_ && camera_->is_open()) {
        camera_->close();
        VCAM_INFO(kModule, "camera closed");
    }
    {
        std::lock_guard<std::mutex> lock(source_mutex_);
        if (source_) {
            source_->close();
        }
    }
    drain_metrics();
    if (timing_csv_.is_open()) {
        timing_csv_.close();
    }
    state_.transition(EngineState::Stopped);  // no-op if in ERROR
}

void Engine::fail(const Status& status, int exit_code) {
    {
        std::lock_guard<std::mutex> lock(info_mutex_);
        if (error_.empty()) {
            error_ = status.message();
        }
    }
    exit_code_.store(exit_code);
    VCAM_ERROR(kModule, status.message());
    finished_.store(true);
    state_.transition(EngineState::Error);
    notify(event_fd_);
}

void Engine::set_message(const std::string& message) {
    std::lock_guard<std::mutex> lock(info_mutex_);
    last_message_ = message;
}

void Engine::update_source_stats() {
    // Called by the loader thread only (the source belongs to it).
    const SourceStats stats = source_->stats();
    std::lock_guard<std::mutex> lock(info_mutex_);
    source_stats_ = stats;
}

// ============================================================================
// Loader thread: LOADING -> BUFFERING -> READY
// ============================================================================

void Engine::loader_main() {
    set_current_thread_name("vcam-loader");
    Status status = prepare();
    if (!status.ok()) {
        if (!stop_.load() && status.code() != StatusCode::Cancelled) {
            // prepare() reports every output-device problem as DeviceError.
            const bool device_problem = status.code() == StatusCode::DeviceError;
            const bool resource_problem = status.code() == StatusCode::ResourceExhausted;
            fail(status, device_problem ? kExitDevice : (resource_problem ? kExitRuntime : kExitInput));
        }
        return;
    }
    if (stop_.load() || !state_.transition(EngineState::Buffering)) {
        return;
    }

    if (live_) {
        receive_live();
        return;
    }
    if (stream_mode_) {
        stream_frames();
        return;
    }

    status = buffer_frames();
    if (!status.ok()) {
        if (!stop_.load()) {
            fail(status, status.code() == StatusCode::ResourceExhausted ? kExitRuntime : kExitInput);
        }
        return;
    }
    if (!stop_.load()) {
        state_.transition(EngineState::Ready);
    }
}

Status Engine::prepare() {
    // ---- 1. Source ----
    SourceSpec spec;
    spec.type = config_.input.type;
    spec.path = config_.input.path;
    spec.fps = config_.input.source_fps;
    spec.pattern_width = config_.input.pattern_width;
    spec.pattern_height = config_.input.pattern_height;
    spec.pattern_frames = config_.input.pattern_frames;
    live_.store(resolve_source_type(spec) == SourceType::Push);
    stream_mode_ = !live_ && config_.buffer.mode == BufferMode::Stream;
    // Decoder threads: in stream mode the decoder only has to keep up with
    // real time, so 2 threads are plenty and leave the other cores alone.
    // When preloading (ram/disk), use every core to finish loading quickly.
    spec.decoder_threads =
        config_.input.decoder_threads >= 0 ? config_.input.decoder_threads : (stream_mode_ ? 2 : 0);
    if (stream_mode_) {
        // Decoding now runs WHILE streaming. Lower this (loader) thread's CPU
        // priority before the decoder starts its own threads, which inherit
        // it: on a busy or small machine the pacer always wins the CPU, and
        // the decoder simply uses what is left (the window absorbs the delay).
        const std::string note = lower_current_thread_priority(10);
        if (!note.empty()) {
            VCAM_DEBUG(kModule, note);
        }
    }

    auto created = create_source(spec);
    if (!created.ok()) {
        return created.status();
    }
    {
        std::lock_guard<std::mutex> lock(source_mutex_);
        source_ = std::move(created.value());
    }
    if (stop_.load()) {
        return Status(StatusCode::Cancelled, "stopped");
    }
    Status status = source_->open();  // may wait for a producer (push input)
    if (!status.ok()) {
        return status;
    }
    const SourceInfo info = source_->info();

    // ---- 2. Output format: source resolution, chosen pixel format ----
    auto format = make_output_format(info, config_.output.pixel_format);
    if (!format.ok()) {
        return format.status();
    }
    output_format_ = format.value();

    if (config_.output.fps) {
        output_fps_ = *config_.output.fps;
    } else if (info.nominal_fps && is_positive(*info.nominal_fps)) {
        output_fps_ = *info.nominal_fps;
    } else {
        output_fps_ = make_rational(30, 1);
        VCAM_WARN(kModule, "source has no frame rate; using 30 fps (set one with --fps)");
    }

    // ---- 3. Output device: check it NOW, before spending time on buffering ----
    std::string description;
    switch (config_.output.backend) {
        case OutputBackend::V4L2: {
            auto device = V4L2LoopbackCamera::probe(config_.output.device);
            if (!device.ok()) {
                return Status(StatusCode::DeviceError, device.status().message());
            }
            camera_ = std::make_unique<V4L2LoopbackCamera>(config_.output.device);
            description = config_.output.device + " ('" + device->card + "')";
            break;
        }
        case OutputBackend::Null:
            camera_ = std::make_unique<NullCamera>();
            description = camera_->description();
            break;
        case OutputBackend::File:
            camera_ = std::make_unique<RawFileCamera>(config_.output.file_path);
            description = camera_->description();
            break;
    }

    // ---- 4. Buffer, with a size check before anything is decoded ----
    const std::optional<uint64_t> expected = info.frame_count_estimate;
    if (live_) {
        buffer_ = std::make_unique<LiveFrameBuffer>(3);
    } else if (stream_mode_) {
        const size_t capacity = stream_capacity(info);
        status = check_fits("A read-ahead window of " + std::to_string(capacity) + " frames of " +
                                describe(output_format_),
                            estimate_buffer_bytes(output_format_, capacity), default_ram_budget(),
                            "Use a smaller --read-ahead or --pixel-format i420.");
        if (!status.ok()) {
            return status;
        }
        auto stream = std::make_unique<StreamBuffer>(capacity);
        stream_ = stream.get();
        buffer_ = std::move(stream);
        source_time_base_ = info.time_base;
        if (info.nominal_fps && is_positive(*info.nominal_fps)) {
            // One source frame in ticks = (1 / fps) / time_base.
            source_frame_ticks_ = rescale(1, invert(*info.nominal_fps), info.time_base);
        }
    } else if (config_.buffer.mode == BufferMode::Ram) {
        const uint64_t budget = config_.buffer.memory_limit ? config_.buffer.memory_limit : default_ram_budget();
        if (expected) {
            status = check_fits("Buffering " + std::to_string(*expected) + " frames of " + describe(output_format_) +
                                    " in RAM",
                                estimate_buffer_bytes(output_format_, *expected), budget, buffer_advice());
            if (!status.ok()) {
                return status;
            }
        }
        auto ram = std::make_unique<RamBuffer>(budget);
        ram->set_time_base(info.time_base);
        buffer_ = std::move(ram);
    } else {
        const std::optional<uint64_t> free_space = free_disk_bytes(config_.buffer.spill_dir);
        if (!free_space) {
            return Status(StatusCode::NotFound, "spill directory '" + config_.buffer.spill_dir + "' is not accessible");
        }
        const uint64_t budget = config_.buffer.memory_limit ? config_.buffer.memory_limit : *free_space / 10 * 9;
        if (expected) {
            status = check_fits("Buffering " + std::to_string(*expected) + " frames on disk in " +
                                    config_.buffer.spill_dir,
                                estimate_buffer_bytes(output_format_, *expected), budget,
                                "Free disk space, choose another --spill-dir or use --pixel-format i420.");
            if (!status.ok()) {
                return status;
            }
        }
        auto disk = std::make_unique<DiskBackedBuffer>(config_.buffer.spill_dir, budget);
        status = disk->open();
        if (!status.ok()) {
            return status;
        }
        disk->set_time_base(info.time_base);
        buffer_ = std::move(disk);
    }
    status = buffer_->configure(output_format_, expected);
    if (!status.ok()) {
        return status;
    }
    status = source_->set_output_format(output_format_);
    if (!status.ok()) {
        return status;
    }

    if (info.duration_ticks && is_positive(info.time_base)) {
        duration_s_.store(static_cast<double>(*info.duration_ticks) * to_double(info.time_base));
    }
    {
        std::lock_guard<std::mutex> lock(info_mutex_);
        source_info_ = info;
        source_known_ = true;
        output_description_ = description;
        // Progress bar while BUFFERING: the whole input, or just the window.
        frames_expected_ = stream_ ? stream_->capacity() : expected.value_or(0);
    }
    VCAM_DEBUG(kModule, "prepared: " << describe(output_format_) << " @ " << to_string(output_fps_) << " fps -> "
                                     << description);
    return Status::ok_status();
}

Status Engine::buffer_frames() {
    uint64_t loaded = 0;
    while (!stop_.load()) {
        auto slot = buffer_->begin_write();
        if (!slot.ok()) {
            return Status(slot.status().code(), slot.status().message() + ". " + buffer_advice());
        }
        FrameTiming timing;
        ReadResult result = source_->read_next(slot.value(), timing);
        if (result.is_frame()) {
            buffer_->commit_write(timing);
            if (++loaded % 16 == 0) {
                update_source_stats();
            }
        } else if (result.is_end_of_stream()) {
            break;
        } else if (result.is_error()) {
            return result.status();
        }
        // Again: nothing yet, try once more
    }
    if (stop_.load()) {
        return Status::ok_status();
    }
    update_source_stats();
    if (loaded == 0) {
        return Status(StatusCode::InvalidData, "no frames could be decoded from the input");
    }
    buffer_->mark_complete();
    {
        std::lock_guard<std::mutex> lock(source_mutex_);
        source_->close();  // the decoder is no longer needed: free its memory
    }
    // Exact length from the decoded timestamps (more reliable than the header).
    const Timeline* timeline = buffer_->timeline();
    if (timeline && !timeline->empty()) {
        duration_s_.store(static_cast<double>(timeline->end_ticks()) * to_double(timeline->time_base()));
    }
    {
        std::lock_guard<std::mutex> lock(info_mutex_);
        frames_expected_ = loaded;
    }
    VCAM_DEBUG(kModule, "buffered " << loaded << " frames, " << format_bytes(buffer_->stats().memory_bytes) << " RAM, "
                                    << format_bytes(buffer_->stats().disk_bytes) << " disk");
    return Status::ok_status();
}

void Engine::receive_live() {
    bool ready = false;
    uint64_t received = 0;
    while (!stop_.load()) {
        auto slot = buffer_->begin_write();
        if (!slot.ok()) {
            // All slots pinned for a moment (cannot normally happen with 3 slots).
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        FrameTiming timing;
        ReadResult result = source_->read_next(slot.value(), timing);
        if (result.is_frame()) {
            buffer_->commit_write(timing);
            if (!ready) {
                ready = true;
                update_source_stats();
                state_.transition(EngineState::Ready);
            }
            if (++received % 30 == 0) {
                update_source_stats();
            }
        } else if (result.is_error()) {
            fail(result.status(), kExitInput);
            return;
        } else if (result.is_end_of_stream()) {
            break;
        }
    }
    update_source_stats();
}

// Stream mode (default): keep a short window of decoded frames ahead of the
// pacer, for as long as the engine runs.
//
//   * window full      -> sleep in wait_for_space() (no CPU used)
//   * end of the video -> loop: rewind the decoder and keep going; timestamps
//                         get `offset` (= length of the video) added, so the
//                         buffer sees one ever-increasing timeline
//                       stop/hold: mark the buffer complete and idle
//   * seek request     -> seek the decoder, empty the window, report back
//
// The engine becomes READY once the window is full for the first time (or the
// whole input fits in it), so streaming starts with a full cushion.
void Engine::stream_frames() {
    bool ready = false;
    const auto become_ready = [&] {
        if (!ready && !stop_.load()) {
            ready = true;
            update_source_stats();
            state_.transition(EngineState::Ready);
        }
    };

    const bool loop = config_.playback.on_eof == EofPolicy::Loop;
    int64_t offset = 0;            // added to every pts (sum of the lengths of completed passes)
    int64_t unwrapped_end = 0;     // end of the last committed frame, offset included
    uint64_t pass = 0;             // how many times the video was restarted by looping
    uint64_t frames_this_pass = 0;
    uint64_t seeks_handled = 0;
    bool source_finished = false;  // stop/hold: the last frame was decoded
    uint64_t decoded = 0;

    while (!stop_.load()) {
        // ---- 1. seek requested by the pacer? ----
        uint64_t requested = 0;
        int64_t target = 0;
        {
            std::lock_guard<std::mutex> lock(seek_mutex_);
            requested = seek_requested_;
            target = seek_target_ticks_;
        }
        if (requested != seeks_handled) {
            Status status = source_->seek(target);
            if (!status.ok()) {
                fail(Status(status.code(), "seek failed: " + status.message()), kExitInput);
                return;
            }
            // The pacer's clock now runs in plain video time again.
            offset = 0;
            unwrapped_end = 0;
            frames_this_pass = 0;
            source_finished = false;
            stream_->restart();
            seeks_handled = requested;
            seek_completed_.store(requested);
            VCAM_DEBUG(kModule, "decoder seeked to tick " << target);
            continue;
        }

        if (source_finished) {
            // stop/hold: nothing more to decode; wait for a seek or for stop.
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }

        // ---- 2. wait for room in the window ----
        if (!stream_->wait_for_space(20)) {
            become_ready();  // window full: enough cushion to start streaming
            continue;        // (re-check stop and seek requests)
        }

        // ---- 3. decode one frame into the window ----
        auto slot = stream_->begin_write();
        if (!slot.ok()) {
            continue;  // cannot happen after wait_for_space(); be safe anyway
        }
        FrameTiming timing;
        ReadResult result = source_->read_next(slot.value(), timing);
        if (result.is_frame()) {
            timing.pts_ticks += offset;
            unwrapped_end = timing.pts_ticks + std::max<int64_t>(timing.duration_ticks, 1);
            stream_->commit_write(timing);
            ++frames_this_pass;
            if (++decoded % 30 == 0) {
                update_source_stats();
            }
        } else if (result.is_end_of_stream()) {
            if (frames_this_pass == 0) {
                fail(Status(StatusCode::InvalidData, "no frames could be decoded from the input"), kExitInput);
                return;
            }
            // Exact length of the video, from the decoded timestamps
            // (`offset` is where this pass started on the unwrapped timeline).
            duration_s_.store(static_cast<double>(unwrapped_end - offset) * to_double(source_time_base_));
            if (loop) {
                offset = unwrapped_end;  // the next pass starts where this one ended
                frames_this_pass = 0;
                ++pass;
                stream_->set_pass(pass);
                Status status = source_->seek(0);
                if (!status.ok()) {
                    fail(Status(status.code(), "cannot rewind the input to loop it: " + status.message()),
                         kExitInput);
                    return;
                }
            } else {
                stream_->mark_complete();
                source_finished = true;
                update_source_stats();
                become_ready();  // the whole (short) input fitted in the window
            }
        } else if (result.is_error()) {
            fail(result.status(), kExitInput);
            return;
        }
        // Again: nothing yet, try once more
    }
    update_source_stats();
}

// ============================================================================
// Streaming
// ============================================================================

Status Engine::start_streaming() {
    if (state_.current() != EngineState::Ready) {
        return Status(StatusCode::InvalidArgument, std::string("cannot start streaming in state ") +
                                                       state_name(state_.current()));
    }
    Status status = camera_->open(output_format_, output_fps_);
    if (!status.ok()) {
        fail(status, kExitDevice);
        return status;
    }
    std::string description = camera_->description();
    if (auto* v4l2 = dynamic_cast<V4L2LoopbackCamera*>(camera_.get())) {
        if (!config_.output.device_name.empty() && v4l2->device_info().card != config_.output.device_name) {
            VCAM_WARN(kModule, "the device is named '" << v4l2->device_info().card << "', not '"
                                                       << config_.output.device_name
                                                       << "'. Rename it with: sudo tools/setup_loopback.sh --reload "
                                                          "--name \""
                                                       << config_.output.device_name << "\"");
        }
    }
    {
        std::lock_guard<std::mutex> lock(info_mutex_);
        output_description_ = description;
    }

    analyzer_ = std::make_unique<TimingAnalyzer>(output_fps_, static_cast<int64_t>(config_.timing.late_ms * 1e6),
                                                 config_.playback.realtime);
    if (!config_.diagnostics.timing_csv.empty()) {
        timing_csv_.open(config_.diagnostics.timing_csv);
        if (timing_csv_) {
            write_frame_csv_header(timing_csv_);
        } else {
            VCAM_WARN(kModule, "cannot write " << config_.diagnostics.timing_csv);
        }
    }

    state_.transition(EngineState::Streaming);
    pacer_ = std::thread(&Engine::pacer_main, this);
    return Status::ok_status();
}

void Engine::post(const Command& command) {
    switch (command.type) {
        case CommandType::Stop:
            request_stop();
            break;
        case CommandType::Start:
            if (state_.current() == EngineState::Ready) {
                start_streaming();
            } else {
                set_message("start: the camera is not in READY state");
            }
            break;
        case CommandType::Pause:
        case CommandType::Resume:
        case CommandType::Seek:
            if (!pacer_.joinable()) {
                set_message("not streaming yet");
            } else {
                mailbox_.post(command);
            }
            break;
        case CommandType::Stats:
        case CommandType::Help:
            break;  // handled by the control loop
    }
}

void Engine::pacer_main() {
    set_current_thread_name("vcam-pacer");

    RealtimeOptions realtime;
    realtime.fifo_priority = config_.timing.rt_priority;
    realtime.lock_memory = config_.timing.lock_memory;
    for (const std::string& note : configure_timing_thread(realtime)) {
        VCAM_WARN(kModule, note);  // once, before the timed loop starts
    }

    MonotonicClock real_clock;
    SimulatedClock simulated_clock(monotonic_now_ns());
    Clock& clock = config_.playback.realtime ? static_cast<Clock&>(real_clock) : simulated_clock;

    SchedulerOptions scheduler_options;
    scheduler_options.spin_ns = static_cast<int64_t>(config_.timing.spin_us) * 1000;
    Scheduler scheduler(clock, output_fps_, scheduler_options);

    // Which source frame to show in each slot:
    //   ram / disk : Playhead over the complete Timeline
    //   stream     : SourceClock (time arithmetic) + StreamBuffer::pick()
    //   live       : newest frame received
    std::optional<Playhead> playhead;
    std::optional<SourceClock> source_clock;
    if (stream_mode_) {
        source_clock.emplace(source_time_base_, output_fps_);
    } else if (!live_) {
        playhead.emplace(*buffer_->timeline(), output_fps_, config_.playback.selection, config_.playback.on_eof);
    }
    // 'nearest' selection in stream mode: the frame whose timestamp is closest
    // to time t is the frame on screen at t + half a frame (exact for constant
    // frame rate videos, a close approximation for variable ones).
    const int64_t nearest_shift =
        config_.playback.selection == SelectionPolicy::Nearest ? source_frame_ticks_ / 2 : 0;
    const bool loop = config_.playback.on_eof == EofPolicy::Loop;

    const uint64_t max_slots =
        config_.playback.duration_s > 0
            ? static_cast<uint64_t>(std::ceil(config_.playback.duration_s * to_double(output_fps_)))
            : 0;
    const bool stop_output_when_paused = config_.playback.pause_output == PauseOutput::Stop;

    scheduler.start();
    const uint64_t first_slot = scheduler.next_index();
    if (playhead) {
        playhead->start(first_slot);
    }
    if (source_clock) {
        source_clock->start(first_slot);
    }

    FrameLease held;           // frame currently on screen (kept for repeats)
    uint64_t held_index = 0;
    bool have_held = false;
    bool paused = false;
    bool at_end = false;
    // Stream mode only:
    bool seeking = false;      // waiting for the loader to refill the window after a seek
    uint64_t pending_seek = 0; // number of the seek request we wait for
    uint64_t shown_pass = 0;   // loop pass of the frame on screen

    // Source time shown in `slot`, in seconds within the video (for display).
    const auto position_seconds = [&](uint64_t slot_index) -> double {
        if (playhead) {
            return playhead->position_seconds(slot_index);
        }
        if (!source_clock) {
            return 0.0;
        }
        const double seconds = source_clock->seconds_at(slot_index);  // grows across loops
        const double duration = duration_s_.load();
        if (duration <= 0) {
            return seconds;
        }
        return loop ? std::fmod(seconds, duration) : std::min(seconds, duration);
    };

    // Stream mode: has the loader finished the seek and decoded a frame?
    const auto seek_ready = [&] {
        return seek_completed_.load() == pending_seek && stream_->buffered_frames() > 0;
    };

    while (!stop_.load()) {
        const SlotTiming slot = scheduler.wait_for_next_slot();
        if (stop_.load()) {
            break;
        }
        if (max_slots > 0 && slot.index - first_slot >= max_slots) {
            break;  // --duration reached
        }

        // ---- commands (cheap check when none is pending) ----
        Command command;
        while (mailbox_.take(command)) {
            char text[128];
            if (command.type == CommandType::Pause && !paused) {
                paused = true;
                if (playhead) playhead->pause(slot.index);
                if (source_clock) source_clock->pause(slot.index);
                state_.transition(EngineState::Paused);
                std::snprintf(text, sizeof(text), "paused at %.3f s", position_seconds(slot.index));
                set_message(text);
            } else if (command.type == CommandType::Resume && paused) {
                paused = false;
                if (playhead) playhead->resume(slot.index);
                if (source_clock && !seeking) source_clock->resume(slot.index);  // a seek resumes when done
                state_.transition(at_end ? EngineState::Eof : EngineState::Streaming);
                set_message("resumed");
            } else if (command.type == CommandType::Seek) {
                if (playhead) {
                    playhead->seek(slot.index, command.seconds);
                    at_end = false;
                    if (state_.current() == EngineState::Eof) {
                        state_.transition(EngineState::Streaming);
                    }
                    std::snprintf(text, sizeof(text), "seeked to %.3f s", position_seconds(slot.index));
                    set_message(text);
                } else if (source_clock) {
                    // Clamp to the video: [0, start of the last frame].
                    double seconds = std::max(command.seconds, 0.0);
                    const double duration = duration_s_.load();
                    const double frame_s = source_frame_ticks_ > 0
                                               ? static_cast<double>(source_frame_ticks_) * to_double(source_time_base_)
                                               : 0.0;
                    if (duration > 0) {
                        seconds = std::min(seconds, std::max(duration - frame_s, 0.0));
                    }
                    // Exact integer arithmetic: position in clock units, the
                    // decoder target in whole ticks (rounded down).
                    const Int128 position = source_clock->seconds_to_units(seconds);
                    const int64_t target = static_cast<int64_t>(position / source_clock->units_per_tick());
                    {
                        // Hand the target to the loader (it owns the decoder).
                        std::lock_guard<std::mutex> lock(seek_mutex_);
                        seek_target_ticks_ = target;
                        pending_seek = ++seek_requested_;
                    }
                    // Freeze the clock at the target until the new frames are decoded;
                    // the frame on screen is repeated meanwhile (no black frames).
                    seeking = true;
                    source_clock->set_position(slot.index, position);
                    source_clock->pause(slot.index);
                    at_end = false;
                    if (state_.current() == EngineState::Eof) {
                        state_.transition(EngineState::Streaming);
                    }
                    std::snprintf(text, sizeof(text), "seeking to %.3f s", seconds);
                    set_message(text);
                } else {
                    set_message("seek is not available for live input");
                }
            }
        }

        // ---- stream mode: finish a seek once the loader has new frames ----
        if (seeking) {
            if (!config_.playback.realtime) {
                // Fast mode: simulated time must not run ahead of the decoder.
                while (!seek_ready() && !stop_.load() && !finished_.load()) {
                    std::this_thread::yield();
                }
            }
            if (seek_ready()) {
                seeking = false;
                if (!paused) {
                    source_clock->resume(slot.index);
                }
                char text[128];
                std::snprintf(text, sizeof(text), "seeked to %.3f s", position_seconds(slot.index));
                set_message(text);
            }
        }

        FrameRecord record;
        record.slot = slot.index;
        record.target_ns = slot.target_ns;
        record.wake_ns = slot.wake_ns;
        record.missed_before = static_cast<uint32_t>(slot.missed_before);
        if (paused) {
            record.flags |= kFramePaused;
        }

        if (paused && stop_output_when_paused) {
            record.flags |= kFrameNotPublished;
            record.publish_ns = clock.now_ns();
        } else {
            if (source_clock) {
                // ---- stream mode: the frame on screen at the clock's time ----
                if (seeking) {
                    if (have_held) record.flags |= kFrameRepeated;  // keep the old frame during a seek
                } else {
                    const int64_t ticks = source_clock->ticks_at(slot.index) + nearest_shift;
                    StreamPick pick = stream_->pick(ticks);
                    if (!config_.playback.realtime) {
                        // Fast mode: simulated time runs much faster than decoding,
                        // so wait for the decoder instead of reporting underflow.
                        // Picking again may jump over the frame picked first, which
                        // was never published: count it as skipped.
                        while ((pick.underflow || pick.waiting) && !stop_.load() && !finished_.load()) {
                            std::this_thread::yield();
                            StreamPick again = stream_->pick(ticks);
                            if (again.new_frame) {
                                again.skipped += pick.skipped + (pick.new_frame ? 1u : 0u);
                            } else {
                                again.skipped = pick.skipped;
                                again.new_frame = pick.new_frame;
                            }
                            pick = std::move(again);
                        }
                    }
                    if (pick.past_end) {
                        if (config_.playback.on_eof == EofPolicy::Stop) {
                            state_.transition(EngineState::Eof);
                            VCAM_INFO(kModule, "end of input reached (on_eof=stop)");
                            break;
                        }
                        if (!at_end) {  // hold: keep showing the last frame
                            at_end = true;
                            if (!paused) state_.transition(EngineState::Eof);
                        }
                    }
                    if (pick.underflow) {
                        record.flags |= kFrameUnderflow;  // decoder behind: previous frame stays longer
                    } else if (!pick.new_frame && have_held) {
                        record.flags |= kFrameRepeated;
                    }
                    if (pick.pass != shown_pass) {
                        record.flags |= kFrameLooped;  // the loop seam reached the screen
                        shown_pass = pick.pass;
                        loops_.store(pick.pass);
                    }
                    record.source_frames_skipped = pick.skipped;
                    if (pick.lease) {
                        held = std::move(pick.lease);
                        held_index = held.view().timing.source_index;
                        have_held = true;
                    }
                }
                position_s_.store(position_seconds(slot.index));
            } else {
                // ---- ram / disk / live: choose the source frame ----
                uint64_t index = 0;
                bool have_index = true;
                if (playhead) {
                    const Selection selection = playhead->select(slot.index);
                    if (selection.end_of_stream) {
                        state_.transition(EngineState::Eof);
                        VCAM_INFO(kModule, "end of input reached (on_eof=stop)");
                        break;
                    }
                    if (selection.at_end && !at_end) {
                        at_end = true;
                        if (!paused) state_.transition(EngineState::Eof);
                    }
                    if (selection.looped) {
                        record.flags |= kFrameLooped;
                        loops_.store(playhead->loops());
                    }
                    index = selection.source_index;
                    record.source_frames_skipped = selection.skipped;
                    position_s_.store(playhead->position_seconds(slot.index));
                } else {
                    const std::optional<uint64_t> newest = buffer_->newest_index();
                    have_index = newest.has_value();
                    index = (paused && have_held) ? held_index : newest.value_or(0);
                    if (have_held && have_index && index > held_index + 1) {
                        record.source_frames_skipped = static_cast<uint32_t>(index - held_index - 1);
                    }
                }

                // ---- get the frame (keep the previous one if it is the same) ----
                if (have_index && (!have_held || index != held_index)) {
                    FrameLease lease = buffer_->acquire(index);
                    if (!lease && live_) {  // overwritten since newest_index(): take the newest again
                        if (auto newest = buffer_->newest_index()) {
                            index = *newest;
                            lease = buffer_->acquire(index);
                        }
                    }
                    if (lease) {
                        held = std::move(lease);
                        held_index = index;
                        have_held = true;
                    } else {
                        record.flags |= kFrameUnderflow;  // repeat the previous frame
                    }
                } else if (have_held) {
                    record.flags |= kFrameRepeated;
                }
            }

            if (!have_held) {
                record.flags |= kFrameNotPublished | kFrameUnderflow;
                record.publish_ns = clock.now_ns();
            } else {
                // ---- deliver ----
                const PublishOutcome outcome = camera_->publish(held.view());
                record.publish_ns = clock.now_ns();
                record.source_index = held_index;
                if (outcome.kind == PublishOutcome::Kind::Ok) {
                    frames_sent_.fetch_add(1, std::memory_order_relaxed);
                } else if (outcome.kind == PublishOutcome::Kind::Backpressure) {
                    record.flags |= kFrameBackpressure;
                } else {
                    record.flags |= kFrameOutputError;
                    if (outcome.fatal) {
                        records_.push(record);
                        fail(outcome.status, kExitDevice);
                        break;
                    }
                }
            }
        }

        // ---- hand the record to the control thread ----
        if (config_.playback.realtime) {
            if (!records_.push(record)) {
                records_lost_.fetch_add(1, std::memory_order_relaxed);  // never block in real time
            }
        } else {
            while (!records_.push(record) && !stop_.load()) {
                std::this_thread::yield();  // fast mode: wait for the control thread instead of losing data
            }
        }
        slots_.fetch_add(1, std::memory_order_relaxed);

        if (config_.playback.max_frames > 0 && frames_sent_.load() >= config_.playback.max_frames) {
            break;  // --max-frames reached
        }
    }

    held.reset();
    finished_.store(true);
    notify(event_fd_);
}

// ============================================================================
// Statistics for the control thread
// ============================================================================

void Engine::drain_metrics() {
    if (!analyzer_) {
        return;
    }
    FrameRecord record;
    while (records_.pop(record)) {
        analyzer_->add(record);
        if (timing_csv_.is_open()) {
            write_frame_csv_row(timing_csv_, record);
        }
    }
}

TimingReport Engine::timing_report() {
    drain_metrics();
    if (!analyzer_) {
        // Not streaming yet. (Do not read output_fps_ here: the loader thread
        // may still be writing it.)
        return TimingReport{};
    }
    return analyzer_->report();
}

EngineSnapshot Engine::snapshot() {
    EngineSnapshot snapshot;
    snapshot.timing = timing_report();
    snapshot.state = state_.current();
    snapshot.live = live_.load();
    {
        std::lock_guard<std::mutex> lock(info_mutex_);
        snapshot.error = error_;
        snapshot.last_message = last_message_;
        last_message_.clear();
        snapshot.source_known = source_known_;
        snapshot.source = source_info_;
        snapshot.source_stats = source_stats_;
        snapshot.output_description = output_description_;
        snapshot.frames_expected = frames_expected_;
    }
    if (snapshot.source_known) {
        snapshot.format_known = true;
        snapshot.output_format = output_format_;
        snapshot.output_fps = output_fps_;
        if (buffer_) {
            snapshot.buffer = buffer_->stats();
        }
    }
    snapshot.position_s = position_s_.load();
    snapshot.duration_s = duration_s_.load();
    snapshot.loops = loops_.load();
    snapshot.frames_sent = frames_sent_.load();
    snapshot.slots = slots_.load();
    snapshot.metric_records_lost = records_lost_.load();
    return snapshot;
}

Status Engine::run_blocking(double timeout_s) {
    Status status = start();
    if (!status.ok()) {
        return status;
    }
    const int64_t deadline = monotonic_now_ns() + static_cast<int64_t>(timeout_s * 1e9);
    while (true) {
        const EngineState current = state_.current();
        if (current == EngineState::Ready && config_.playback.start == StartMode::Auto) {
            start_streaming();
        }
        drain_metrics();
        if (finished_.load() || is_terminal(current) || stop_.load()) {
            break;
        }
        if (monotonic_now_ns() > deadline) {
            VCAM_WARN(kModule, "run_blocking: timeout");
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    shutdown();
    if (state_.current() == EngineState::Error) {
        std::lock_guard<std::mutex> lock(info_mutex_);
        return Status(StatusCode::Internal, error_);
    }
    return Status::ok_status();
}

}  // namespace vcam
