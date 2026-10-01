#include "vcam/config/command_line.hpp"

#include <getopt.h>

#include <cstdio>
#include <vector>

#include "vcam/config/config_file.hpp"

namespace vcam {

namespace {

// Option name -> config key (nullptr key = handled specially).
struct OptionMapping {
    const char* name;
    int has_argument;   // required_argument / no_argument
    const char* key;
    const char* flag_value;  // value used for flags without argument
};

const std::vector<OptionMapping>& mappings() {
    static const std::vector<OptionMapping> table = {
        {"input", required_argument, "input.path", nullptr},
        {"source-type", required_argument, "input.type", nullptr},
        {"source-fps", required_argument, "input.source_fps", nullptr},
        {"pattern-size", required_argument, "input.pattern_size", nullptr},
        {"pattern-frames", required_argument, "input.pattern_frames", nullptr},
        {"socket", required_argument, "input.path", nullptr},
        {"output", required_argument, "output.backend", nullptr},
        {"device", required_argument, "output.device", nullptr},
        {"device-name", required_argument, "output.device_name", nullptr},
        {"output-file", required_argument, "output.file", nullptr},
        {"fps", required_argument, "output.fps", nullptr},
        {"pixel-format", required_argument, "output.pixel_format", nullptr},
        {"buffer-mode", required_argument, "buffer.mode", nullptr},
        {"read-ahead", required_argument, "buffer.read_ahead", nullptr},
        {"decoder-threads", required_argument, "input.decoder_threads", nullptr},
        {"memory-limit", required_argument, "buffer.memory_limit", nullptr},
        {"spill-dir", required_argument, "buffer.spill_dir", nullptr},
        {"realtime", required_argument, "playback.realtime", nullptr},
        {"fast", no_argument, "playback.realtime", "false"},
        {"on-eof", required_argument, "playback.on_eof", nullptr},
        {"selection", required_argument, "playback.selection", nullptr},
        {"start", required_argument, "playback.start", nullptr},
        {"pause-output", required_argument, "playback.pause_output", nullptr},
        {"duration", required_argument, "playback.duration", nullptr},
        {"max-frames", required_argument, "playback.max_frames", nullptr},
        {"rt-priority", required_argument, "timing.rt_priority", nullptr},
        {"spin-us", required_argument, "timing.spin_us", nullptr},
        {"lock-memory", no_argument, "timing.lock_memory", "true"},
        {"late-ms", required_argument, "timing.late_ms", nullptr},
        {"log-level", required_argument, "log.level", nullptr},
        {"verbose", no_argument, "log.level", "debug"},
        {"quiet", no_argument, "diagnostics.progress", "false"},
        {"stats-interval", required_argument, "diagnostics.stats_interval", nullptr},
        {"stats-json", required_argument, "diagnostics.stats_json", nullptr},
        {"timing-csv", required_argument, "diagnostics.timing_csv", nullptr},
        {"report-json", required_argument, "diagnostics.report_json", nullptr},
        {"no-stdin", no_argument, "control.stdin", "false"},
    };
    return table;
}

constexpr int kConfigOption = 1000;
constexpr int kSetOption = 1001;
constexpr int kHelpOption = 'h';
constexpr int kVersionOption = 1002;
constexpr int kFirstMapped = 2000;

}  // namespace

std::string usage_text(const char* program) {
    std::string text;
    text += "Usage: ";
    text += program;
    text += " --input <video | image-directory | pattern> [options]\n"
            "       ";
    text += program;
    text += " --source-type push [--socket /tmp/vcam.sock] [options]\n\n"
            "Turns a video, an image sequence, a test pattern or frames pushed by a script into a\n"
            "Linux webcam (/dev/videoN via v4l2loopback). The camera resolution is the source's.\n\n"
            "Common options:\n"
            "  -i, --input PATH        video file, image directory, or 'pattern'\n"
            "      --source-type T     auto | video | images | pattern | push\n"
            "  -d, --device PATH       v4l2loopback device (default /dev/video10)\n"
            "      --fps RATE          camera frame rate: 30, 29.97, 30000/1001 (default: source rate)\n"
            "      --pixel-format F    yuyv (default) | uyvy | i420 | nv12 | rgb24 | bgr24 | gray8\n"
            "      --on-eof P          loop (default) | stop | hold\n"
            "      --buffer-mode M     stream (default: low RAM) | ram | disk (decode everything first)\n"
            "      --output B          v4l2 (default) | null | file  (null/file: no device needed)\n"
            "      --fast              fast simulation mode (simulated clock, needs an end condition)\n"
            "      --duration S        stop after S seconds of streaming\n"
            "      --config FILE       read settings from a YAML file (command line overrides it)\n"
            "      --set KEY=VALUE     set any key below directly\n"
            "  -v, --verbose           debug logging\n"
            "  -q, --quiet             no live progress/status lines\n"
            "  -h, --help              this help        --version   version\n\n"
            "Interactive commands while running (type + Enter): pause, resume, seek <seconds>, stats, start, stop\n\n"
            "All settings (config-file key / --set key, values, meaning):\n";
    char line[256];
    for (const SettingHelp& help : setting_help()) {
        std::snprintf(line, sizeof(line), "  %-28s %-24s %s\n", help.key, help.values, help.description);
        text += line;
    }
    text += "\nOption -> key: ";
    bool first = true;
    for (const OptionMapping& mapping : mappings()) {
        text += (first ? "" : ", ");
        text += std::string("--") + mapping.name;
        first = false;
    }
    text += " map to the keys above.\n";
    return text;
}

Result<CommandLine> parse_command_line(int argc, char** argv) {
    // Build getopt_long's option table from the mapping table.
    std::vector<option> options;
    for (size_t i = 0; i < mappings().size(); ++i) {
        options.push_back({mappings()[i].name, mappings()[i].has_argument, nullptr, kFirstMapped + static_cast<int>(i)});
    }
    options.push_back({"config", required_argument, nullptr, kConfigOption});
    options.push_back({"set", required_argument, nullptr, kSetOption});
    options.push_back({"help", no_argument, nullptr, kHelpOption});
    options.push_back({"version", no_argument, nullptr, kVersionOption});
    options.push_back({nullptr, 0, nullptr, 0});

    // Collect everything first, so the config file can be applied before the
    // other options regardless of their order on the command line.
    CommandLine result;
    std::string config_file;
    std::vector<std::pair<std::string, std::string>> settings;

    optind = 1;  // allow repeated parsing (tests)
    opterr = 0;  // we print our own errors
    int letter = 0;
    while ((letter = getopt_long(argc, argv, ":i:d:vqh", options.data(), nullptr)) != -1) {
        if (letter == 'i') {
            settings.emplace_back("input.path", optarg);
        } else if (letter == 'd') {
            settings.emplace_back("output.device", optarg);
        } else if (letter == 'v') {
            settings.emplace_back("log.level", "debug");
        } else if (letter == 'q') {
            settings.emplace_back("diagnostics.progress", "false");
        } else if (letter == kHelpOption) {
            result.show_help = true;
        } else if (letter == kVersionOption) {
            result.show_version = true;
        } else if (letter == kConfigOption) {
            config_file = optarg;
        } else if (letter == kSetOption) {
            const std::string assignment = optarg;
            const size_t equals = assignment.find('=');
            if (equals == std::string::npos) {
                return Status(StatusCode::InvalidArgument, "--set expects key=value, got '" + assignment + "'");
            }
            settings.emplace_back(assignment.substr(0, equals), assignment.substr(equals + 1));
        } else if (letter >= kFirstMapped) {
            const OptionMapping& mapping = mappings()[static_cast<size_t>(letter - kFirstMapped)];
            settings.emplace_back(mapping.key, mapping.has_argument == no_argument ? mapping.flag_value : optarg);
        } else if (letter == ':') {
            return Status(StatusCode::InvalidArgument, std::string("option ") + argv[optind - 1] + " needs a value");
        } else {
            return Status(StatusCode::InvalidArgument,
                          std::string("unknown option ") + argv[optind - 1] + " (see --help)");
        }
    }
    if (optind < argc) {
        // A bare argument is accepted as the input path: `virtual-camera video.mp4`.
        if (optind == argc - 1) {
            settings.emplace_back("input.path", argv[optind]);
        } else {
            return Status(StatusCode::InvalidArgument, std::string("unexpected argument '") + argv[optind + 1] + "'");
        }
    }

    if (!config_file.empty()) {
        Status status = load_config_file(config_file, result.config);
        if (!status.ok()) {
            return status;
        }
    }
    for (const auto& [key, value] : settings) {
        Status status = apply_setting(result.config, key, value);
        if (!status.ok()) {
            return status;
        }
    }
    return result;
}

}  // namespace vcam
