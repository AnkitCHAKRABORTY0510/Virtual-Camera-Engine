// =============================================================================
// engine.hpp — wires all modules together into a running virtual camera
//
//   loader thread                       pacer thread (timing-critical)
//   ─────────────                       ─────────────────────────────
//   LOADING: open source, choose        each slot n (Scheduler, absolute deadline):
//            format, check device,        apply pause/resume/seek (mailbox)
//            check memory                 pick source frame (SourceClock+StreamBuffer,
//   BUFFERING: decode -> buffer             Playhead, or newest live frame)
//   READY                                 publish to the VirtualCamera
//   stream mode: keeps decoding just      push a FrameRecord to the metrics ring
//     ahead of the pacer (and seeks)
//   live input: keeps receiving
//
// Three ways of feeding the pacer (chosen in prepare()):
//   * stream (default): StreamBuffer, a ~1 s decode-ahead window. Low RAM.
//   * ram / disk: the whole input is decoded first, then played by Playhead.
//   * live (push input): LiveFrameBuffer, newest frame wins.
//
//   control thread (the caller: ControlLoop or tests): start(), then
//   start_streaming() when READY, post() commands, snapshot() for display,
//   request_stop() + shutdown() at the end.
//
// Responsibilities stay separate: the engine never decodes, schedules or
// writes to V4L2 itself — it only connects Source, Buffer, Playhead,
// Scheduler, VirtualCamera and Metrics.
// =============================================================================
#pragma once

#include <atomic>
#include <cstdint>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "vcam/buffer/frame_buffer.hpp"
#include "vcam/buffer/stream_buffer.hpp"
#include "vcam/config/config.hpp"
#include "vcam/control/commands.hpp"
#include "vcam/control/state_machine.hpp"
#include "vcam/metrics/spsc_ring.hpp"
#include "vcam/metrics/timing_metrics.hpp"
#include "vcam/output/virtual_camera.hpp"
#include "vcam/source/frame_source.hpp"

namespace vcam {

// Exit codes (also used by the virtual-camera app).
constexpr int kExitOk = 0;
constexpr int kExitConfig = 1;
constexpr int kExitInput = 2;
constexpr int kExitDevice = 3;
constexpr int kExitRuntime = 4;

// Everything the control thread wants to display, copied consistently.
struct EngineSnapshot {
    EngineState state = EngineState::Init;
    std::string error;
    std::string last_message;      // reply to the last command (e.g. "seeked to 12.0 s")

    bool source_known = false;
    SourceInfo source;
    SourceStats source_stats;
    bool live = false;

    bool format_known = false;
    FrameFormat output_format{};
    Rational output_fps{30, 1};
    std::string output_description;

    BufferStats buffer;
    uint64_t frames_expected = 0;  // for the progress bar (0 = unknown)

    double position_s = 0;         // source time currently shown
    double duration_s = 0;         // stream length (0 = unknown / live)
    uint64_t loops = 0;
    uint64_t frames_sent = 0;
    uint64_t slots = 0;
    uint64_t metric_records_lost = 0;
    TimingReport timing;
};

class Engine {
public:
    explicit Engine(Config config);
    ~Engine();

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // Starts the loader thread (LOADING -> BUFFERING -> READY). Returns at once.
    Status start();

    // READY -> STREAMING: opens the camera and starts the pacer thread.
    Status start_streaming();

    // pause / resume / seek go to the pacer; start/stop are handled here.
    void post(const Command& command);

    // Asks every thread to finish (Ctrl+C, 'stop', end of run). Non-blocking.
    void request_stop();
    // Joins the threads, closes the camera, ends in STOPPED (or stays in ERROR).
    void shutdown();

    EngineState state() const { return state_.current(); }
    // True when streaming ended by itself (EOF with on_eof=stop, --duration,
    // --max-frames, or a fatal error): the control thread should shut down.
    bool finished() const { return finished_.load(); }
    bool stop_requested() const { return stop_.load(); }

    // A file descriptor that becomes readable whenever the state changes
    // (an eventfd), so the control loop can poll() it with stdin and signals.
    int event_fd() const { return event_fd_; }
    void clear_event();

    // Control thread only: moves timing records from the pacer into the analyzer.
    void drain_metrics();
    EngineSnapshot snapshot();
    TimingReport timing_report();

    int exit_code() const { return exit_code_.load(); }
    const Config& config() const { return config_; }

    // Convenience driver without a terminal (tests, scripts): start, stream
    // when READY, run until finished or `timeout_s`, shut down.
    Status run_blocking(double timeout_s);

private:
    void loader_main();
    void pacer_main();
    Status prepare();          // LOADING
    Status buffer_frames();    // BUFFERING (ram / disk: decode everything)
    void receive_live();       // live sources: BUFFERING + receive forever
    void stream_frames();      // stream mode: BUFFERING + decode ahead forever
    size_t stream_capacity(const SourceInfo& info) const;
    void fail(const Status& status, int exit_code);
    void set_message(const std::string& message);
    void update_source_stats();

    Config config_;
    StateMachine state_;
    int event_fd_ = -1;

    std::atomic<bool> stop_{false};
    std::atomic<bool> finished_{false};
    std::atomic<int> exit_code_{kExitOk};

    std::thread loader_;
    std::thread pacer_;

    // Created by the loader during LOADING (published by the state change).
    std::mutex source_mutex_;  // guards the pointer for interrupt() from other threads
    std::unique_ptr<FrameSource> source_;
    std::unique_ptr<FrameBuffer> buffer_;
    std::unique_ptr<VirtualCamera> camera_;
    FrameFormat output_format_{};
    Rational output_fps_{30, 1};
    std::atomic<bool> live_{false};  // read by snapshot() while the loader sets it

    // ---- stream mode (set in prepare(), before the pacer exists) ----
    bool stream_mode_ = false;
    StreamBuffer* stream_ = nullptr;   // same object as buffer_, typed
    Rational source_time_base_{1, 1};
    int64_t source_frame_ticks_ = 0;   // nominal frame duration (for 'nearest' selection)
    // Seek hand-over pacer -> loader: the pacer sets the target and bumps
    // seek_requested_; the loader seeks the decoder, empties the window and
    // stores the same number in seek_completed_.
    std::mutex seek_mutex_;
    int64_t seek_target_ticks_ = 0;    // guarded by seek_mutex_
    uint64_t seek_requested_ = 0;      // guarded by seek_mutex_
    std::atomic<uint64_t> seek_completed_{0};

    CommandMailbox mailbox_;
    SpscRing<FrameRecord> records_{1 << 16};
    std::atomic<uint64_t> records_lost_{0};
    std::unique_ptr<TimingAnalyzer> analyzer_;  // control thread only
    std::ofstream timing_csv_;

    // Live values published by the threads for snapshot().
    std::atomic<uint64_t> frames_sent_{0};
    std::atomic<uint64_t> slots_{0};
    std::atomic<uint64_t> loops_{0};
    std::atomic<double> position_s_{0};
    std::atomic<double> duration_s_{0};

    mutable std::mutex info_mutex_;  // guards the fields below
    std::string error_;
    std::string last_message_;
    bool source_known_ = false;
    SourceInfo source_info_;
    SourceStats source_stats_;
    std::string output_description_;
    uint64_t frames_expected_ = 0;
};

}  // namespace vcam
