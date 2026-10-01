// =============================================================================
// timing_metrics.hpp — measure how closely output follows the ideal camera clock
//
// For every output frame n (docs/ARCHITECTURE.md §6.4):
//
//   target  T_n   when the frame was due
//   wake    W_n   when the pacer thread woke up
//   actual  A_n   when write() to the device returned (frame visible to apps)
//   lateness  E_n = A_n − T_n
//   interval  I_n = A_n − A_{n−1}
//   jitter    J_n = I_n − k·ΔT   (k = slots between the two frames, normally 1)
//
// Reported: mean/median/P95/P99/max of lateness and |jitter|, measured FPS,
// drift (least-squares slope of E_n over time, in ppm: a non-zero slope means
// error accumulates), and event counters. Expected events (frames repeated or
// skipped because source and output FPS differ) are counted separately from
// unexpected ones (late wake-ups, missing frames, output errors).
// =============================================================================
#pragma once

#include <cstdint>
#include <iosfwd>
#include <string>

#include "vcam/core/rational.hpp"
#include "vcam/metrics/histogram.hpp"

namespace vcam {

// Bit flags describing what happened in one output slot.
enum FrameFlag : uint32_t {
    kFrameRepeated = 1u << 0,        // same source frame as the previous slot (expected: FPS conversion / pause / hold)
    kFrameUnderflow = 1u << 1,       // wanted frame not available, previous one repeated (unexpected)
    kFrameBackpressure = 1u << 2,    // device refused the frame (EAGAIN) — not delivered
    kFrameOutputError = 1u << 3,     // write failed
    kFramePaused = 1u << 4,          // engine paused (frame held)
    kFrameNotPublished = 1u << 5,    // nothing written in this slot (e.g. pause with output stopped)
    kFrameLooped = 1u << 6,          // source wrapped around to frame 0 in this slot
};

// One record per output slot, produced by the pacer thread (48 bytes, trivially copyable).
struct FrameRecord {
    uint64_t slot = 0;
    int64_t target_ns = 0;
    int64_t wake_ns = 0;
    int64_t publish_ns = 0;
    uint64_t source_index = 0;
    uint32_t missed_before = 0;          // output slots skipped right before this one
    uint32_t source_frames_skipped = 0;  // source frames jumped over (expected when output FPS < source FPS)
    uint32_t flags = 0;
};

struct DurationStats {
    double mean_ns = 0;
    double stddev_ns = 0;
    int64_t min_ns = 0;
    int64_t median_ns = 0;
    int64_t p95_ns = 0;
    int64_t p99_ns = 0;
    int64_t max_ns = 0;
};

struct TimingReport {
    Rational requested_fps{0, 1};
    bool realtime = true;
    uint64_t frames_published = 0;      // frames handed to the device
    uint64_t slots = 0;                 // slots processed (published or not)
    double duration_s = 0;              // first to last publish
    double measured_fps = 0;            // (published - 1) / duration

    DurationStats lateness;             // E_n = A_n − T_n
    DurationStats wake_latency;         // W_n − T_n (operating-system part of lateness)
    DurationStats jitter_abs;           // |J_n|
    double drift_ppm = 0;               // slope of lateness over time
    int64_t late_threshold_ns = 0;
    uint64_t late_frames = 0;           // lateness above threshold

    // Unexpected events
    uint64_t missed_slots = 0;          // scheduler overruns: slots never emitted
    uint64_t underflow_frames = 0;
    uint64_t backpressure_frames = 0;
    uint64_t output_errors = 0;
    // Expected events
    uint64_t repeated_frames = 0;       // duplicated source frames (FPS up-conversion, hold, pause)
    uint64_t source_frames_skipped = 0; // dropped source frames (FPS down-conversion)
    uint64_t loops = 0;
};

class TimingAnalyzer {
public:
    TimingAnalyzer(Rational requested_fps, int64_t late_threshold_ns, bool realtime);

    void add(const FrameRecord& record);
    TimingReport report() const;
    void reset();

    uint64_t frames_published() const { return published_; }

private:
    Rational fps_;
    int64_t period_ns_;
    int64_t late_threshold_ns_;
    bool realtime_;

    LatencyHistogram lateness_;
    LatencyHistogram wake_;
    LatencyHistogram jitter_;

    uint64_t slots_ = 0;
    uint64_t published_ = 0;
    int64_t first_publish_ns_ = 0;
    int64_t last_publish_ns_ = 0;
    uint64_t last_published_slot_ = 0;
    uint64_t late_ = 0;
    uint64_t missed_ = 0, underflow_ = 0, backpressure_ = 0, errors_ = 0;
    uint64_t repeated_ = 0, skipped_ = 0, loops_ = 0;

    // Online least-squares regression of lateness (y, ns) on time (x, s).
    int64_t first_target_ns_ = 0;
    uint64_t regression_n_ = 0;
    double mean_x_ = 0, mean_y_ = 0, cov_xy_ = 0, var_x_ = 0;
};

// Multi-line human-readable report.
std::string format_report_text(const TimingReport& report);
// One JSON object (no external JSON library needed for this flat structure).
std::string format_report_json(const TimingReport& report);

// Per-frame CSV for offline analysis.
void write_frame_csv_header(std::ostream& out);
void write_frame_csv_row(std::ostream& out, const FrameRecord& record);

}  // namespace vcam
