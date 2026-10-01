// =============================================================================
// resample_table — Phase 4 tool: show which source frame each output slot gets
//
// Builds a constant-frame-rate timeline and runs the real Playhead over it.
//
// Examples:
//   resample_table --source-fps 24 --output-fps 30 --slots 20
//   resample_table --source-fps 30 --output-fps 15 --frames 10 --eof loop --slots 12
//   resample_table --source-fps 29.97 --output-fps 30 --slots 2005 --only-events
// =============================================================================
#include <getopt.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "vcam/resample/playhead.hpp"

namespace {

bool parse_fps(const char* text, vcam::Rational& out) {
    auto value = vcam::parse_rational(text);
    if (!value.ok() || !vcam::is_positive(value.value())) {
        std::fprintf(stderr, "invalid rate '%s'\n", text);
        return false;
    }
    out = value.value();
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    vcam::Rational source_fps{30, 1};
    vcam::Rational output_fps{30, 1};
    vcam::Rational time_base{1, 90000};
    unsigned long frames = 60;
    unsigned long slots = 30;
    vcam::EofPolicy eof = vcam::EofPolicy::Stop;
    vcam::SelectionPolicy policy = vcam::SelectionPolicy::Hold;
    bool only_events = false;

    static const option kOptions[] = {
        {"source-fps", required_argument, nullptr, 's'}, {"output-fps", required_argument, nullptr, 'o'},
        {"frames", required_argument, nullptr, 'f'},     {"slots", required_argument, nullptr, 'n'},
        {"time-base", required_argument, nullptr, 't'},  {"eof", required_argument, nullptr, 'e'},
        {"policy", required_argument, nullptr, 'p'},     {"only-events", no_argument, nullptr, 'x'},
        {"help", no_argument, nullptr, 'h'},             {nullptr, 0, nullptr, 0},
    };
    int letter = 0;
    while ((letter = getopt_long(argc, argv, "h", kOptions, nullptr)) != -1) {
        switch (letter) {
            case 's': if (!parse_fps(optarg, source_fps)) return 1; break;
            case 'o': if (!parse_fps(optarg, output_fps)) return 1; break;
            case 't': if (!parse_fps(optarg, time_base)) return 1; break;
            case 'f': frames = std::strtoul(optarg, nullptr, 10); break;
            case 'n': slots = std::strtoul(optarg, nullptr, 10); break;
            case 'e': {
                auto value = vcam::parse_eof_policy(optarg);
                if (!value.ok()) { std::fprintf(stderr, "%s\n", value.status().message().c_str()); return 1; }
                eof = value.value();
                break;
            }
            case 'p': {
                auto value = vcam::parse_selection_policy(optarg);
                if (!value.ok()) { std::fprintf(stderr, "%s\n", value.status().message().c_str()); return 1; }
                policy = value.value();
                break;
            }
            case 'x': only_events = true; break;
            default:
                std::printf("Usage: %s [--source-fps R] [--output-fps R] [--frames N] [--slots N]\n"
                            "          [--time-base 1/90000] [--eof stop|loop|hold] [--policy hold|nearest]\n"
                            "          [--only-events]   (print only repeated/skipped/looped slots)\n",
                            argv[0]);
                return letter == 'h' ? 0 : 1;
        }
    }

    const vcam::Timeline timeline = vcam::Timeline::make_cfr(source_fps, time_base, frames);
    vcam::Playhead playhead(timeline, output_fps, policy, eof);
    playhead.start(0);

    std::printf("source %s fps (%lu frames, %.3f s), output %s fps, policy %s, eof %s\n\n",
                vcam::to_string(source_fps).c_str(), frames, playhead.duration_seconds(),
                vcam::to_string(output_fps).c_str(), vcam::selection_policy_name(policy), vcam::eof_policy_name(eof));
    std::printf("%8s %12s %12s %8s  %s\n", "slot", "t_out (s)", "t_src (s)", "frame", "event");

    unsigned long repeats = 0;
    unsigned long skipped = 0;
    for (unsigned long n = 0; n < slots; ++n) {
        const vcam::Selection selection = playhead.select(n);
        const double t_source = playhead.position_seconds(n);  // after select(): includes loop wrap
        const double t_output = static_cast<double>(n) / vcam::to_double(output_fps);
        if (selection.end_of_stream) {
            std::printf("%8lu %12.6f %12.6f %8s  end of stream (eof=stop)\n", n, t_output, t_source, "-");
            break;
        }
        std::string event;
        if (selection.looped) event += "looped ";
        if (selection.repeated) { event += "repeated "; ++repeats; }
        if (selection.skipped) { event += "skipped " + std::to_string(selection.skipped) + " "; skipped += selection.skipped; }
        if (selection.at_end) event += "holding-last ";
        if (!only_events || !event.empty()) {
            std::printf("%8lu %12.6f %12.6f %8llu  %s\n", n, t_output, t_source,
                        static_cast<unsigned long long>(selection.source_index), event.c_str());
        }
    }
    std::printf("\nrepeated slots: %lu, skipped source frames: %lu, loops: %llu\n", repeats, skipped,
                static_cast<unsigned long long>(playhead.loops()));
    return 0;
}
