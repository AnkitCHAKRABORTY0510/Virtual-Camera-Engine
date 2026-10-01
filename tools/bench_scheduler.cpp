// =============================================================================
// bench_scheduler — Phase 3 tool: measure timing accuracy WITHOUT any device
//
// Runs the real scheduler loop for N seconds at the requested FPS. Each slot
// copies one frame-sized block of memory (the same work write() to
// /dev/videoX does) and records target / wake / publish times. Prints the
// timing report: lateness, jitter, drift, measured FPS, overruns.
//
// Examples:
//   bench_scheduler --fps 30 --seconds 60
//   bench_scheduler --fps 60 --seconds 30 --spin-us 200 --rt-priority 10
//   bench_scheduler --fps 120 --seconds 10 --csv frames.csv --json report.json
//   bench_scheduler --simulated --fps 30 --seconds 3600   (fast mode: exactness check)
// =============================================================================
#include <getopt.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "vcam/core/clock_time.hpp"
#include "vcam/core/log.hpp"
#include "vcam/metrics/timing_metrics.hpp"
#include "vcam/timing/clock.hpp"
#include "vcam/timing/realtime.hpp"
#include "vcam/timing/scheduler.hpp"

namespace {

struct Options {
    vcam::Rational fps{30, 1};
    double seconds = 10;
    size_t frame_bytes = 1280 * 720 * 2;  // one 720p YUYV frame
    int spin_us = 0;
    int rt_priority = 0;
    bool simulated = false;
    double late_ms = 2.0;
    std::string csv_path;
    std::string json_path;
};

void usage(const char* program) {
    std::printf(
        "Usage: %s [options]\n"
        "  --fps RATE          output rate: 30, 29.97, 30000/1001 (default 30)\n"
        "  --seconds S         duration (default 10)\n"
        "  --frame-bytes N     bytes copied per frame to simulate output (default 1843200)\n"
        "  --spin-us N         busy-wait the last N µs before each deadline (default 0)\n"
        "  --rt-priority N     SCHED_FIFO priority 1..99 (needs permission)\n"
        "  --late-ms X         'late frame' threshold (default 2.0)\n"
        "  --simulated         use the simulated clock (fast mode)\n"
        "  --csv PATH          per-frame records\n"
        "  --json PATH         report as JSON\n",
        program);
}

bool parse(int argc, char** argv, Options& o) {
    static const option kOptions[] = {
        {"fps", required_argument, nullptr, 'f'},        {"seconds", required_argument, nullptr, 's'},
        {"frame-bytes", required_argument, nullptr, 'b'}, {"spin-us", required_argument, nullptr, 'p'},
        {"rt-priority", required_argument, nullptr, 'r'}, {"late-ms", required_argument, nullptr, 'l'},
        {"simulated", no_argument, nullptr, 'S'},          {"csv", required_argument, nullptr, 'c'},
        {"json", required_argument, nullptr, 'j'},         {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0},
    };
    int letter = 0;
    while ((letter = getopt_long(argc, argv, "h", kOptions, nullptr)) != -1) {
        switch (letter) {
            case 'f': {
                auto fps = vcam::parse_rational(optarg);
                if (!fps.ok() || !vcam::is_positive(fps.value())) {
                    std::fprintf(stderr, "invalid --fps\n");
                    return false;
                }
                o.fps = fps.value();
                break;
            }
            case 's': o.seconds = std::atof(optarg); break;
            case 'b': o.frame_bytes = std::strtoull(optarg, nullptr, 10); break;
            case 'p': o.spin_us = std::atoi(optarg); break;
            case 'r': o.rt_priority = std::atoi(optarg); break;
            case 'l': o.late_ms = std::atof(optarg); break;
            case 'S': o.simulated = true; break;
            case 'c': o.csv_path = optarg; break;
            case 'j': o.json_path = optarg; break;
            default: usage(argv[0]); return false;
        }
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parse(argc, argv, options)) {
        return 1;
    }

    vcam::set_current_thread_name("vcam-bench");
    vcam::RealtimeOptions realtime;
    realtime.fifo_priority = options.rt_priority;
    for (const std::string& note : vcam::configure_timing_thread(realtime)) {
        std::fprintf(stderr, "note: %s\n", note.c_str());
    }

    vcam::MonotonicClock real_clock;
    vcam::SimulatedClock simulated_clock(vcam::monotonic_now_ns());
    vcam::Clock& clock = options.simulated ? static_cast<vcam::Clock&>(simulated_clock) : real_clock;

    vcam::SchedulerOptions scheduler_options;
    scheduler_options.spin_ns = static_cast<int64_t>(options.spin_us) * 1000;
    vcam::Scheduler scheduler(clock, options.fps, scheduler_options);

    const auto slots = static_cast<uint64_t>(options.seconds * vcam::to_double(options.fps));
    std::vector<uint8_t> source(options.frame_bytes, 0x80);
    std::vector<uint8_t> sink(options.frame_bytes);
    // Records are stored in memory during the run and written afterwards:
    // no file I/O inside the timed loop.
    std::vector<vcam::FrameRecord> records;
    records.reserve(slots);

    std::printf("bench_scheduler: %s fps, %.1f s (%llu slots), %zu bytes/frame, %s clock, spin %d us\n",
                vcam::to_string(options.fps).c_str(), options.seconds, static_cast<unsigned long long>(slots),
                options.frame_bytes, options.simulated ? "simulated" : "monotonic", options.spin_us);
    std::fflush(stdout);

    scheduler.start();
    while (records.size() < slots) {
        const vcam::SlotTiming slot = scheduler.wait_for_next_slot();
        std::memcpy(sink.data(), source.data(), sink.size());  // stand-in for write() to the device
        vcam::FrameRecord record;
        record.slot = slot.index;
        record.target_ns = slot.target_ns;
        record.wake_ns = slot.wake_ns;
        record.publish_ns = clock.now_ns();
        record.source_index = slot.index;
        record.missed_before = static_cast<uint32_t>(slot.missed_before);
        records.push_back(record);
    }

    vcam::TimingAnalyzer analyzer(options.fps, static_cast<int64_t>(options.late_ms * 1e6), !options.simulated);
    for (const vcam::FrameRecord& record : records) {
        analyzer.add(record);
    }
    const vcam::TimingReport report = analyzer.report();
    std::printf("\n%s", vcam::format_report_text(report).c_str());

    if (!options.csv_path.empty()) {
        std::ofstream csv(options.csv_path);
        vcam::write_frame_csv_header(csv);
        for (const vcam::FrameRecord& record : records) {
            vcam::write_frame_csv_row(csv, record);
        }
    }
    if (!options.json_path.empty()) {
        std::ofstream(options.json_path) << vcam::format_report_json(report) << '\n';
    }
    return 0;
}
