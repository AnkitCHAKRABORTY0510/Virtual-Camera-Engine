#include "vcam/metrics/timing_metrics.hpp"

#include <cmath>
#include <cstdio>
#include <ostream>
#include <string>

#include "vcam/core/clock_time.hpp"

namespace vcam {

namespace {

constexpr uint32_t kNotDelivered = kFrameNotPublished | kFrameBackpressure | kFrameOutputError;

DurationStats summarize(const LatencyHistogram& histogram) {
    DurationStats stats;
    stats.mean_ns = histogram.mean_ns();
    stats.stddev_ns = histogram.stddev_ns();
    stats.min_ns = histogram.min_ns();
    stats.median_ns = histogram.percentile_ns(50);
    stats.p95_ns = histogram.percentile_ns(95);
    stats.p99_ns = histogram.percentile_ns(99);
    stats.max_ns = histogram.max_ns();
    return stats;
}

double ms(double nanoseconds) {
    return nanoseconds / 1e6;
}

std::string duration_line(const char* label, const DurationStats& s) {
    char line[256];
    std::snprintf(line, sizeof(line),
                  "  %-20s: mean %.3f | median %.3f | P95 %.3f | P99 %.3f | max %.3f ms (std dev %.3f ms)\n", label,
                  ms(s.mean_ns), ms(static_cast<double>(s.median_ns)), ms(static_cast<double>(s.p95_ns)),
                  ms(static_cast<double>(s.p99_ns)), ms(static_cast<double>(s.max_ns)), ms(s.stddev_ns));
    return line;
}

std::string json_duration(const char* name, const DurationStats& s) {
    char text[384];
    std::snprintf(text, sizeof(text),
                  "\"%s\":{\"mean_ns\":%.1f,\"stddev_ns\":%.1f,\"min_ns\":%lld,\"median_ns\":%lld,"
                  "\"p95_ns\":%lld,\"p99_ns\":%lld,\"max_ns\":%lld}",
                  name, s.mean_ns, s.stddev_ns, static_cast<long long>(s.min_ns), static_cast<long long>(s.median_ns),
                  static_cast<long long>(s.p95_ns), static_cast<long long>(s.p99_ns),
                  static_cast<long long>(s.max_ns));
    return text;
}

}  // namespace

TimingAnalyzer::TimingAnalyzer(Rational requested_fps, int64_t late_threshold_ns, bool realtime)
    : fps_(requested_fps),
      period_ns_(rescale(1, invert(requested_fps), Rational{1, kNanosPerSecond}, Rounding::Nearest)),
      late_threshold_ns_(late_threshold_ns),
      realtime_(realtime) {}

void TimingAnalyzer::reset() {
    *this = TimingAnalyzer(fps_, late_threshold_ns_, realtime_);
}

void TimingAnalyzer::add(const FrameRecord& record) {
    ++slots_;
    missed_ += record.missed_before;
    skipped_ += record.source_frames_skipped;
    if (record.flags & kFrameRepeated) ++repeated_;
    if (record.flags & kFrameUnderflow) ++underflow_;
    if (record.flags & kFrameBackpressure) ++backpressure_;
    if (record.flags & kFrameOutputError) ++errors_;
    if (record.flags & kFrameLooped) ++loops_;

    wake_.add(record.wake_ns - record.target_ns);

    if ((record.flags & kNotDelivered) != 0) {
        return;  // nothing reached the device in this slot
    }

    const int64_t lateness = record.publish_ns - record.target_ns;
    lateness_.add(lateness);
    if (lateness > late_threshold_ns_) {
        ++late_;
    }

    if (published_ == 0) {
        first_publish_ns_ = record.publish_ns;
        first_target_ns_ = record.target_ns;
    } else {
        // Interval between this and the previous delivered frame, compared with
        // the ideal distance between their slots (k periods, exact).
        const uint64_t slot_gap = record.slot - last_published_slot_;
        const int64_t ideal = rescale(static_cast<int64_t>(slot_gap), invert(fps_), Rational{1, kNanosPerSecond},
                                      Rounding::Nearest);
        const int64_t jitter = (record.publish_ns - last_publish_ns_) - ideal;
        jitter_.add(jitter < 0 ? -jitter : jitter);
    }
    last_publish_ns_ = record.publish_ns;
    last_published_slot_ = record.slot;
    ++published_;

    // Online linear regression (Welford form): slope of lateness vs. time.
    const double x = static_cast<double>(record.target_ns - first_target_ns_) / 1e9;  // seconds
    const double y = static_cast<double>(lateness);                                   // ns
    ++regression_n_;
    const double dx = x - mean_x_;
    mean_x_ += dx / static_cast<double>(regression_n_);
    mean_y_ += (y - mean_y_) / static_cast<double>(regression_n_);
    cov_xy_ += dx * (y - mean_y_);
    var_x_ += dx * (x - mean_x_);
}

TimingReport TimingAnalyzer::report() const {
    TimingReport report;
    report.requested_fps = fps_;
    report.realtime = realtime_;
    report.frames_published = published_;
    report.slots = slots_;
    if (published_ > 1) {
        report.duration_s = static_cast<double>(last_publish_ns_ - first_publish_ns_) / 1e9;
        if (report.duration_s > 0) {
            report.measured_fps = static_cast<double>(published_ - 1) / report.duration_s;
        }
    }
    report.lateness = summarize(lateness_);
    report.wake_latency = summarize(wake_);
    report.jitter_abs = summarize(jitter_);
    // slope in ns of lateness per second of stream; 1 ppm = 1 µs/s = 1000 ns/s.
    report.drift_ppm = var_x_ > 0 ? (cov_xy_ / var_x_) / 1000.0 : 0.0;
    report.late_threshold_ns = late_threshold_ns_;
    report.late_frames = late_;
    report.missed_slots = missed_;
    report.underflow_frames = underflow_;
    report.backpressure_frames = backpressure_;
    report.output_errors = errors_;
    report.repeated_frames = repeated_;
    report.source_frames_skipped = skipped_;
    report.loops = loops_;
    return report;
}

std::string format_report_text(const TimingReport& r) {
    std::string text;
    char line[256];
    text += r.realtime ? "Timing report (real-time mode)\n" : "Timing report (fast simulation mode: simulated clock)\n";
    std::snprintf(line, sizeof(line), "  %-20s: %.3f (%s)\n", "Requested FPS", to_double(r.requested_fps),
                  to_string(r.requested_fps).c_str());
    text += line;
    std::snprintf(line, sizeof(line), "  %-20s: %.4f\n", "Measured FPS", r.measured_fps);
    text += line;
    std::snprintf(line, sizeof(line), "  %-20s: %llu frames in %.3f s (%llu slots)\n", "Published",
                  static_cast<unsigned long long>(r.frames_published), r.duration_s,
                  static_cast<unsigned long long>(r.slots));
    text += line;
    text += duration_line("Lateness (A - T)", r.lateness);
    text += duration_line("Wake latency (W - T)", r.wake_latency);
    text += duration_line("Jitter |I - dT|", r.jitter_abs);
    if (r.duration_s >= 30.0) {
        std::snprintf(line, sizeof(line), "  %-20s: %+.3f ppm (slope of lateness; ~0 = no accumulating drift)\n",
                      "Drift", r.drift_ppm);
    } else {
        // Over a few seconds, random wake-up noise dominates the slope.
        std::snprintf(line, sizeof(line), "  %-20s: %+.3f ppm (not meaningful for runs under 30 s)\n", "Drift",
                      r.drift_ppm);
    }
    text += line;
    std::snprintf(line, sizeof(line), "  %-20s: %llu (lateness > %.1f ms)\n", "Late frames",
                  static_cast<unsigned long long>(r.late_frames), ms(static_cast<double>(r.late_threshold_ns)));
    text += line;
    auto counter = [&](const char* label, uint64_t value, const char* note) {
        std::snprintf(line, sizeof(line), "  %-20s: %llu%s\n", label, static_cast<unsigned long long>(value), note);
        text += line;
    };
    counter("Dropped (overrun)", r.missed_slots, "  (output slots skipped because the pacer woke too late)");
    counter("Underflow repeats", r.underflow_frames, "  (frame not ready, previous repeated)");
    counter("Backpressure", r.backpressure_frames, "  (device refused frame)");
    counter("Output errors", r.output_errors, "");
    counter("Repeated frames", r.repeated_frames, "  (expected: FPS up-conversion, pause, hold)");
    counter("Skipped src frames", r.source_frames_skipped, "  (expected: output FPS below source FPS)");
    counter("Loops", r.loops, "");
    return text;
}

std::string format_report_json(const TimingReport& r) {
    char head[512];
    std::snprintf(head, sizeof(head),
                  "{\"requested_fps\":%.6f,\"requested_fps_rational\":\"%s\",\"realtime\":%s,"
                  "\"frames_published\":%llu,\"slots\":%llu,\"duration_s\":%.6f,\"measured_fps\":%.6f,",
                  to_double(r.requested_fps), to_string(r.requested_fps).c_str(), r.realtime ? "true" : "false",
                  static_cast<unsigned long long>(r.frames_published), static_cast<unsigned long long>(r.slots),
                  r.duration_s, r.measured_fps);
    char tail[640];
    std::snprintf(tail, sizeof(tail),
                  ",\"drift_ppm\":%.6f,\"late_threshold_ns\":%lld,\"late_frames\":%llu,\"missed_slots\":%llu,"
                  "\"underflow_frames\":%llu,\"backpressure_frames\":%llu,\"output_errors\":%llu,"
                  "\"repeated_frames\":%llu,\"source_frames_skipped\":%llu,\"loops\":%llu}",
                  r.drift_ppm, static_cast<long long>(r.late_threshold_ns),
                  static_cast<unsigned long long>(r.late_frames), static_cast<unsigned long long>(r.missed_slots),
                  static_cast<unsigned long long>(r.underflow_frames),
                  static_cast<unsigned long long>(r.backpressure_frames),
                  static_cast<unsigned long long>(r.output_errors), static_cast<unsigned long long>(r.repeated_frames),
                  static_cast<unsigned long long>(r.source_frames_skipped), static_cast<unsigned long long>(r.loops));
    return std::string(head) + json_duration("lateness", r.lateness) + "," +
           json_duration("wake_latency", r.wake_latency) + "," + json_duration("jitter_abs", r.jitter_abs) + tail;
}

void write_frame_csv_header(std::ostream& out) {
    out << "slot,target_ns,wake_ns,publish_ns,lateness_ns,source_index,missed_before,source_frames_skipped,flags\n";
}

void write_frame_csv_row(std::ostream& out, const FrameRecord& r) {
    out << r.slot << ',' << r.target_ns << ',' << r.wake_ns << ',' << r.publish_ns << ','
        << (r.publish_ns - r.target_ns) << ',' << r.source_index << ',' << r.missed_before << ','
        << r.source_frames_skipped << ',' << r.flags << '\n';
}

}  // namespace vcam
