// =============================================================================
// probe_source — Phase 1 debugging tool for the `source` module
//
// Opens a video file through VideoFileSource (exactly the code path the engine
// will use), prints its metadata, decodes every frame into the chosen output
// pixel format and reports counts, timestamps, frame-rate analysis, decode
// speed and the RAM a full preload would need.
//
// Examples:
//   probe_source video.mp4
//   probe_source --frames all --csv timestamps.csv video.mp4
//   probe_source --pixel-format rgb24 --dump frames.raw --max-frames 30 video.mp4
//
// Exit codes: 0 ok, 1 usage error, 2 input error, 4 decode error, 130 Ctrl+C.
// =============================================================================
#include <getopt.h>  // getopt_long: POSIX command-line parsing
#include <signal.h>  // sigaction

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <string>

#include "vcam/core/clock_time.hpp"
#include "vcam/core/frame.hpp"
#include "vcam/core/log.hpp"
#include "vcam/source/timestamp_stats.hpp"
#include "vcam/source/video_file_source.hpp"

namespace {

constexpr int kExitOk = 0;
constexpr int kExitUsage = 1;
constexpr int kExitInput = 2;
constexpr int kExitRuntime = 4;
constexpr int kExitInterrupted = 130;

// Set by the SIGINT handler, polled by the decode loop. A lock-free atomic
// is one of the few things that is safe to touch inside a signal handler.
std::atomic<bool> g_interrupted{false};

void on_interrupt(int /*signal*/) {
    g_interrupted.store(true);
}

struct Options {
    std::string input;
    vcam::PixelFormat pixel_format = vcam::PixelFormat::YUYV;
    long long frames_to_print = 10;  // -1 = all
    std::optional<unsigned long long> max_frames;
    std::string csv_path;
    std::string dump_path;
};

void print_usage(const char* program) {
    std::printf(
        "Usage: %s [options] <video-file>\n"
        "\n"
        "Decode a video file with the engine's VideoFileSource and report what it finds.\n"
        "\n"
        "Options:\n"
        "  -p, --pixel-format FMT  output pixel format: %s (default yuyv)\n"
        "  -f, --frames N|all      print timing of the first N frames (default 10, 0 = none)\n"
        "  -m, --max-frames N      stop after N frames\n"
        "  -c, --csv PATH          write per-frame timing as CSV (index,pts_ticks,pts_seconds,duration_ticks)\n"
        "  -d, --dump PATH         write converted raw frames to PATH (view with the printed ffplay command)\n"
        "  -l, --log-level LEVEL   error, warn, info, debug, trace (default warn)\n"
        "  -h, --help              show this help\n",
        program, vcam::supported_pixel_format_names().c_str());
}

// Returns the exit code to use, or -1 when parsing succeeded.
int parse_options(int argc, char** argv, Options& options) {
    // getopt_long walks argv; each entry maps a long name to a short letter.
    static const option kLongOptions[] = {
        {"pixel-format", required_argument, nullptr, 'p'},
        {"frames", required_argument, nullptr, 'f'},
        {"max-frames", required_argument, nullptr, 'm'},
        {"csv", required_argument, nullptr, 'c'},
        {"dump", required_argument, nullptr, 'd'},
        {"log-level", required_argument, nullptr, 'l'},
        {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0},
    };

    vcam::log::set_level(vcam::LogLevel::Warn);
    int letter = 0;
    while ((letter = getopt_long(argc, argv, "p:f:m:c:d:l:h", kLongOptions, nullptr)) != -1) {
        switch (letter) {
            case 'p': {
                auto format = vcam::parse_pixel_format(optarg);
                if (!format.ok()) {
                    std::fprintf(stderr, "error: %s\n", format.status().message().c_str());
                    return kExitUsage;
                }
                options.pixel_format = format.value();
                break;
            }
            case 'f':
                options.frames_to_print = std::string(optarg) == "all" ? -1 : std::atoll(optarg);
                break;
            case 'm':
                options.max_frames = std::strtoull(optarg, nullptr, 10);
                break;
            case 'c':
                options.csv_path = optarg;
                break;
            case 'd':
                options.dump_path = optarg;
                break;
            case 'l': {
                vcam::LogLevel level{};
                if (!vcam::log::parse_level(optarg, level)) {
                    std::fprintf(stderr, "error: unknown log level '%s'\n", optarg);
                    return kExitUsage;
                }
                vcam::log::set_level(level);
                break;
            }
            case 'h':
                print_usage(argv[0]);
                return kExitOk;
            default:
                print_usage(argv[0]);
                return kExitUsage;
        }
    }
    if (optind != argc - 1) {
        print_usage(argv[0]);
        return kExitUsage;
    }
    options.input = argv[optind];
    return -1;
}

double ticks_to_seconds(int64_t ticks, vcam::Rational time_base) {
    return static_cast<double>(ticks) * vcam::to_double(time_base);
}

// FFmpeg's name for our pixel format (for the ffplay hint only).
const char* ffplay_pixel_format(vcam::PixelFormat format) {
    switch (format) {
        case vcam::PixelFormat::YUYV:  return "yuyv422";
        case vcam::PixelFormat::UYVY:  return "uyvy422";
        case vcam::PixelFormat::I420:  return "yuv420p";
        case vcam::PixelFormat::NV12:  return "nv12";
        case vcam::PixelFormat::RGB24: return "rgb24";
        case vcam::PixelFormat::BGR24: return "bgr24";
        case vcam::PixelFormat::GRAY8: return "gray";
    }
    return "?";
}

void print_info(const vcam::SourceInfo& info, const vcam::FrameFormat& output) {
    std::printf("Source\n");
    std::printf("  file            : %s\n", info.uri.c_str());
    std::printf("  container       : %s\n", info.container_name.c_str());
    std::printf("  codec           : %s (%s)\n", info.codec_name.c_str(), info.native_pixel_format.c_str());
    std::printf("  resolution      : %dx%d\n", info.width, info.height);
    std::printf("  time base       : %s s\n", vcam::to_string(info.time_base).c_str());
    if (info.nominal_fps) {
        std::printf("  nominal fps     : %s (%.3f)\n", vcam::to_string(*info.nominal_fps).c_str(),
                    vcam::to_double(*info.nominal_fps));
    } else {
        std::printf("  nominal fps     : unknown\n");
    }
    if (info.duration_ticks) {
        std::printf("  duration        : %.3f s\n", ticks_to_seconds(*info.duration_ticks, info.time_base));
    }
    if (info.frame_count_estimate) {
        std::printf("  frames (header) : %llu (container estimate)\n",
                    static_cast<unsigned long long>(*info.frame_count_estimate));
    }
    std::printf("  VFR suspected   : %s\n", info.variable_frame_rate_suspected ? "yes" : "no");
    std::printf("Output (resolution = source resolution)\n");
    std::printf("  format          : %s\n", vcam::describe(output).c_str());
    std::printf("\n");
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    const int parse_result = parse_options(argc, argv, options);
    if (parse_result >= 0) {
        return parse_result;
    }

    // Ctrl+C sets a flag; the loop finishes the current frame and reports.
    struct sigaction action {};
    action.sa_handler = on_interrupt;
    sigaction(SIGINT, &action, nullptr);

    // ---- Open ---------------------------------------------------------------------
    vcam::VideoFileSource source(options.input);
    vcam::Status status = source.open();
    if (!status.ok()) {
        std::fprintf(stderr, "error: %s\n", status.to_string().c_str());
        return kExitInput;
    }
    const vcam::SourceInfo& info = source.info();

    auto output_format = vcam::make_output_format(info, options.pixel_format);
    if (!output_format.ok()) {
        std::fprintf(stderr, "error: %s\n", output_format.status().to_string().c_str());
        return kExitInput;
    }
    status = source.set_output_format(output_format.value());
    if (!status.ok()) {
        std::fprintf(stderr, "error: %s\n", status.to_string().c_str());
        return kExitInput;
    }
    print_info(info, output_format.value());
    std::fflush(stdout);  // show the header before any decode warnings (stderr is unbuffered)

    // ---- Optional output files -----------------------------------------------------
    std::ofstream csv;
    if (!options.csv_path.empty()) {
        csv.open(options.csv_path);
        if (!csv) {
            std::fprintf(stderr, "error: cannot write '%s'\n", options.csv_path.c_str());
            return kExitUsage;
        }
        csv << "index,pts_ticks,pts_seconds,duration_ticks\n";
    }
    std::ofstream dump;
    if (!options.dump_path.empty()) {
        dump.open(options.dump_path, std::ios::binary);
        if (!dump) {
            std::fprintf(stderr, "error: cannot write '%s'\n", options.dump_path.c_str());
            return kExitUsage;
        }
    }

    // ---- Decode loop ---------------------------------------------------------------
    vcam::OwnedFrame frame(output_format.value());  // one reusable frame buffer
    vcam::TimestampStats timestamps;
    bool fatal_error = false;

    if (options.frames_to_print != 0) {
        std::printf("Frames\n  %8s %12s %12s %12s %10s\n", "index", "pts_ticks", "pts_s", "delta_ms", "dur_ticks");
    }

    const int64_t start_ns = vcam::monotonic_now_ns();
    int64_t previous_pts = 0;
    while (!g_interrupted.load()) {
        if (options.max_frames && timestamps.count() >= *options.max_frames) {
            break;
        }
        vcam::FrameTiming timing;
        vcam::ReadResult result = source.read_next(frame.mutable_view(), timing);
        if (result.is_end_of_stream()) {
            break;
        }
        if (result.is_error()) {
            std::fprintf(stderr, "error: %s\n", result.status().to_string().c_str());
            fatal_error = true;
            break;
        }

        const double pts_seconds = ticks_to_seconds(timing.pts_ticks, info.time_base);
        const bool print_this = options.frames_to_print < 0 ||
                                timing.source_index < static_cast<uint64_t>(options.frames_to_print);
        if (print_this) {
            const double delta_ms =
                timing.source_index == 0 ? 0.0 : ticks_to_seconds(timing.pts_ticks - previous_pts, info.time_base) * 1e3;
            std::printf("  %8llu %12lld %12.6f %12.3f %10lld\n", static_cast<unsigned long long>(timing.source_index),
                        static_cast<long long>(timing.pts_ticks), pts_seconds, delta_ms,
                        static_cast<long long>(timing.duration_ticks));
        }
        if (csv) {
            csv << timing.source_index << ',' << timing.pts_ticks << ',' << pts_seconds << ','
                << timing.duration_ticks << '\n';
        }
        if (dump) {
            const auto bytes = frame.view().bytes;
            dump.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        }
        timestamps.add(timing.pts_ticks);
        previous_pts = timing.pts_ticks;
    }
    const double elapsed_seconds =
        static_cast<double>(vcam::monotonic_now_ns() - start_ns) / static_cast<double>(vcam::kNanosPerSecond);
    const vcam::SourceStats stats = source.stats();
    source.close();

    // ---- Summary ---------------------------------------------------------------------
    const uint64_t frames = timestamps.count();
    std::printf("\nSummary\n");
    std::printf("  frames decoded  : %llu\n", static_cast<unsigned long long>(frames));
    std::printf("  decode errors   : %llu (skipped packets)\n", static_cast<unsigned long long>(stats.decode_errors));
    std::printf("  damaged frames  : %llu (delivered with concealed errors)\n",
                static_cast<unsigned long long>(stats.corrupt_frames));
    std::printf("  timestamp fixes : %llu\n", static_cast<unsigned long long>(stats.timestamp_repairs));
    if (frames >= 2) {
        const double min_ms = ticks_to_seconds(timestamps.min_interval(), info.time_base) * 1e3;
        const double max_ms = ticks_to_seconds(timestamps.max_interval(), info.time_base) * 1e3;
        const double mean_ms = timestamps.mean_interval().value() * vcam::to_double(info.time_base) * 1e3;
        std::printf("  first..last pts : %.6f .. %.6f s\n", ticks_to_seconds(timestamps.first_pts(), info.time_base),
                    ticks_to_seconds(timestamps.last_pts(), info.time_base));
        std::printf("  frame interval  : min %.3f ms, mean %.3f ms, max %.3f ms\n", min_ms, mean_ms, max_ms);
        std::printf("  measured fps    : %.4f\n", timestamps.measured_fps(info.time_base).value_or(0.0));
        // Allow one tick of container rounding (e.g. 33/34 ms in a 1/1000 time base).
        std::printf("  frame rate type : %s\n", timestamps.is_constant_frame_rate(1) ? "CFR (constant intervals)"
                                                                       : "VFR (variable intervals: real VFR or missing frames)");
    }
    const double decode_fps = elapsed_seconds > 0 ? static_cast<double>(frames) / elapsed_seconds : 0.0;
    std::printf("  decode speed    : %.1f frames/s (%.2f s total)\n", decode_fps, elapsed_seconds);
    const double preload_mib =
        static_cast<double>(frames) * static_cast<double>(output_format->size_bytes) / (1024.0 * 1024.0);
    std::printf("  RAM for preload : %.1f MiB (%llu frames x %zu bytes)\n", preload_mib,
                static_cast<unsigned long long>(frames), output_format->size_bytes);

    if (dump) {
        const double rate = info.nominal_fps ? vcam::to_double(*info.nominal_fps) : 30.0;
        std::printf("\nView the dump with:\n  ffplay -f rawvideo -pixel_format %s -video_size %dx%d -framerate %.3f %s\n",
                    ffplay_pixel_format(options.pixel_format), info.width, info.height, rate, options.dump_path.c_str());
    }

    if (g_interrupted.load()) {
        std::printf("\nInterrupted by Ctrl+C (partial results above).\n");
        return kExitInterrupted;
    }
    return fatal_error ? kExitRuntime : kExitOk;
}
