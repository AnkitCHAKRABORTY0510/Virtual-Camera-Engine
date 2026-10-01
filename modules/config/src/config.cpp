#include "vcam/config/config.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <functional>
#include <map>

#include "vcam/core/text_format.hpp"

namespace vcam {

namespace {

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

Status bad_value(const std::string& key, const std::string& value, const std::string& expected) {
    return Status(StatusCode::InvalidArgument, "invalid value '" + value + "' for " + key + " (expected " + expected + ")");
}

bool parse_bool(const std::string& text, bool& out) {
    const std::string value = lower(text);
    if (value == "true" || value == "yes" || value == "on" || value == "1") { out = true; return true; }
    if (value == "false" || value == "no" || value == "off" || value == "0") { out = false; return true; }
    return false;
}

bool parse_double(const std::string& text, double& out) {
    char* end = nullptr;
    out = std::strtod(text.c_str(), &end);
    return end != text.c_str() && *end == '\0';
}

bool parse_int(const std::string& text, long long& out) {
    char* end = nullptr;
    out = std::strtoll(text.c_str(), &end, 10);
    return end != text.c_str() && *end == '\0';
}

// "1280x720" -> width, height
bool parse_size(const std::string& text, int& width, int& height) {
    const size_t x = lower(text).find('x');
    if (x == std::string::npos) return false;
    long long w = 0, h = 0;
    if (!parse_int(text.substr(0, x), w) || !parse_int(text.substr(x + 1), h)) return false;
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384) return false;
    width = static_cast<int>(w);
    height = static_cast<int>(h);
    return true;
}

// A setter receives the value and stores it in the config (or reports an error).
using Setter = std::function<Status(Config&, const std::string& key, const std::string& value)>;

struct SettingEntry {
    SettingHelp help;
    Setter setter;
};

Status set_rate(const std::string& key, const std::string& value, Rational& out) {
    auto rate = parse_rational(value);
    if (!rate.ok() || !is_positive(rate.value())) {
        return bad_value(key, value, "a positive rate such as 30, 29.97 or 30000/1001");
    }
    out = rate.value();
    return Status::ok_status();
}

const std::vector<SettingEntry>& entries() {
    static const std::vector<SettingEntry> table = {
        // ---- input ----
        {{"input.path", "PATH", "video file, image directory, 'pattern', or socket path for push input"},
         [](Config& c, const std::string&, const std::string& v) { c.input.path = v; return Status::ok_status(); }},
        {{"input.type", "auto|video|images|pattern|push", "source type (auto: directory=images, 'pattern', else video)"},
         [](Config& c, const std::string& k, const std::string& v) {
             auto type = parse_source_type(lower(v));
             if (!type.ok()) return bad_value(k, v, "auto, video, images, pattern or push");
             c.input.type = type.value();
             return Status::ok_status();
         }},
        {{"input.source_fps", "RATE", "frame rate of image sequences and the pattern (default 30)"},
         [](Config& c, const std::string& k, const std::string& v) { return set_rate(k, v, c.input.source_fps); }},
        {{"input.pattern_size", "WxH", "test pattern resolution (default 1280x720)"},
         [](Config& c, const std::string& k, const std::string& v) {
             if (!parse_size(v, c.input.pattern_width, c.input.pattern_height)) return bad_value(k, v, "WIDTHxHEIGHT");
             return Status::ok_status();
         }},
        {{"input.pattern_frames", "N", "length of the test pattern in frames (default 300)"},
         [](Config& c, const std::string& k, const std::string& v) {
             long long n = 0;
             if (!parse_int(v, n) || n <= 0) return bad_value(k, v, "a positive integer");
             c.input.pattern_frames = static_cast<uint64_t>(n);
             return Status::ok_status();
         }},
        {{"input.decoder_threads", "N|auto", "video decoder threads (auto: 2 in stream mode, all cores when preloading)"},
         [](Config& c, const std::string& k, const std::string& v) {
             if (lower(v) == "auto") { c.input.decoder_threads = -1; return Status::ok_status(); }
             long long n = 0;
             if (!parse_int(v, n) || n < 0 || n > 64) return bad_value(k, v, "auto or 0..64 (0 = all cores)");
             c.input.decoder_threads = static_cast<int>(n);
             return Status::ok_status();
         }},
        // ---- output ----
        {{"output.backend", "v4l2|null|file", "where frames go: the v4l2loopback device, nowhere, or a raw file"},
         [](Config& c, const std::string& k, const std::string& v) {
             const std::string value = lower(v);
             if (value == "v4l2") c.output.backend = OutputBackend::V4L2;
             else if (value == "null") c.output.backend = OutputBackend::Null;
             else if (value == "file") c.output.backend = OutputBackend::File;
             else return bad_value(k, v, "v4l2, null or file");
             return Status::ok_status();
         }},
        {{"output.device", "PATH", "v4l2loopback device (default /dev/video10)"},
         [](Config& c, const std::string&, const std::string& v) { c.output.device = v; return Status::ok_status(); }},
        {{"output.device_name", "NAME", "expected camera name; set it with tools/setup_loopback.sh --name"},
         [](Config& c, const std::string&, const std::string& v) { c.output.device_name = v; return Status::ok_status(); }},
        {{"output.file", "PATH", "raw output file for output.backend=file"},
         [](Config& c, const std::string&, const std::string& v) { c.output.file_path = v; return Status::ok_status(); }},
        {{"output.fps", "RATE|source", "camera frame rate (default: the source's rate)"},
         [](Config& c, const std::string& k, const std::string& v) {
             if (lower(v) == "source" || v.empty()) { c.output.fps.reset(); return Status::ok_status(); }
             Rational rate{};
             Status status = set_rate(k, v, rate);
             if (status.ok()) c.output.fps = rate;
             return status;
         }},
        {{"output.pixel_format", "yuyv|uyvy|i420|nv12|rgb24|bgr24|gray8", "camera pixel format (default yuyv)"},
         [](Config& c, const std::string& k, const std::string& v) {
             auto format = parse_pixel_format(v);
             if (!format.ok()) return bad_value(k, v, supported_pixel_format_names());
             c.output.pixel_format = format.value();
             return Status::ok_status();
         }},
        // ---- buffer ----
        {{"buffer.mode", "stream|ram|disk",
          "stream (default): decode just ahead, low RAM; ram/disk: decode the whole input first"},
         [](Config& c, const std::string& k, const std::string& v) {
             const std::string value = lower(v);
             if (value == "stream") c.buffer.mode = BufferMode::Stream;
             else if (value == "ram") c.buffer.mode = BufferMode::Ram;
             else if (value == "disk") c.buffer.mode = BufferMode::Disk;
             else return bad_value(k, v, "stream, ram or disk");
             return Status::ok_status();
         }},
        {{"buffer.read_ahead", "SECONDS", "stream mode: how much video is decoded ahead (default 1.0)"},
         [](Config& c, const std::string& k, const std::string& v) {
             if (!parse_double(v, c.buffer.read_ahead_s) || c.buffer.read_ahead_s < 0.1 ||
                 c.buffer.read_ahead_s > 60) {
                 return bad_value(k, v, "seconds between 0.1 and 60");
             }
             return Status::ok_status();
         }},
        {{"buffer.memory_limit", "SIZE|auto", "budget for buffered frames, e.g. 4G (default: half of free RAM / free disk)"},
         [](Config& c, const std::string& k, const std::string& v) {
             if (lower(v) == "auto") { c.buffer.memory_limit = 0; return Status::ok_status(); }
             if (!parse_bytes(v, c.buffer.memory_limit)) return bad_value(k, v, "a size such as 512M or 4G");
             return Status::ok_status();
         }},
        {{"buffer.spill_dir", "DIR", "directory for the disk buffer's temporary file (default /var/tmp)"},
         [](Config& c, const std::string&, const std::string& v) { c.buffer.spill_dir = v; return Status::ok_status(); }},
        // ---- playback ----
        {{"playback.realtime", "true|false", "false = fast simulation mode (no real-time pacing)"},
         [](Config& c, const std::string& k, const std::string& v) {
             if (!parse_bool(v, c.playback.realtime)) return bad_value(k, v, "true or false");
             return Status::ok_status();
         }},
        {{"playback.on_eof", "stop|loop|hold", "what happens after the last frame (default loop)"},
         [](Config& c, const std::string& k, const std::string& v) {
             auto policy = parse_eof_policy(lower(v));
             if (!policy.ok()) return bad_value(k, v, "stop, loop or hold");
             c.playback.on_eof = policy.value();
             return Status::ok_status();
         }},
        {{"playback.selection", "hold|nearest", "source frame choice when rates differ (default hold)"},
         [](Config& c, const std::string& k, const std::string& v) {
             auto policy = parse_selection_policy(lower(v));
             if (!policy.ok()) return bad_value(k, v, "hold or nearest");
             c.playback.selection = policy.value();
             return Status::ok_status();
         }},
        {{"playback.start", "auto|manual", "manual = wait in READY for the 'start' command"},
         [](Config& c, const std::string& k, const std::string& v) {
             const std::string value = lower(v);
             if (value == "auto") c.playback.start = StartMode::Auto;
             else if (value == "manual") c.playback.start = StartMode::Manual;
             else return bad_value(k, v, "auto or manual");
             return Status::ok_status();
         }},
        {{"playback.pause_output", "hold|stop", "while paused: keep sending the held frame, or send nothing"},
         [](Config& c, const std::string& k, const std::string& v) {
             const std::string value = lower(v);
             if (value == "hold") c.playback.pause_output = PauseOutput::Hold;
             else if (value == "stop") c.playback.pause_output = PauseOutput::Stop;
             else return bad_value(k, v, "hold or stop");
             return Status::ok_status();
         }},
        {{"playback.duration", "SECONDS", "stop after this much streaming time (0 = unlimited)"},
         [](Config& c, const std::string& k, const std::string& v) {
             if (!parse_double(v, c.playback.duration_s) || c.playback.duration_s < 0) return bad_value(k, v, "seconds >= 0");
             return Status::ok_status();
         }},
        {{"playback.max_frames", "N", "stop after this many output frames (0 = unlimited)"},
         [](Config& c, const std::string& k, const std::string& v) {
             long long n = 0;
             if (!parse_int(v, n) || n < 0) return bad_value(k, v, "an integer >= 0");
             c.playback.max_frames = static_cast<uint64_t>(n);
             return Status::ok_status();
         }},
        // ---- timing ----
        {{"timing.rt_priority", "0-99", "SCHED_FIFO priority for the timing thread (needs permission)"},
         [](Config& c, const std::string& k, const std::string& v) {
             long long n = 0;
             if (!parse_int(v, n) || n < 0 || n > 99) return bad_value(k, v, "0..99");
             c.timing.rt_priority = static_cast<int>(n);
             return Status::ok_status();
         }},
        {{"timing.spin_us", "N", "busy-wait the last N microseconds before each frame (default 0)"},
         [](Config& c, const std::string& k, const std::string& v) {
             long long n = 0;
             if (!parse_int(v, n) || n < 0 || n > 10000) return bad_value(k, v, "0..10000");
             c.timing.spin_us = static_cast<int>(n);
             return Status::ok_status();
         }},
        {{"timing.lock_memory", "true|false", "mlockall(): keep buffered frames from being swapped out"},
         [](Config& c, const std::string& k, const std::string& v) {
             if (!parse_bool(v, c.timing.lock_memory)) return bad_value(k, v, "true or false");
             return Status::ok_status();
         }},
        {{"timing.late_ms", "MS", "lateness counted as a 'late frame' (default 2.0)"},
         [](Config& c, const std::string& k, const std::string& v) {
             if (!parse_double(v, c.timing.late_ms) || c.timing.late_ms <= 0) return bad_value(k, v, "milliseconds > 0");
             return Status::ok_status();
         }},
        // ---- diagnostics ----
        {{"log.level", "error|warn|info|debug|trace", "log verbosity (default info)"},
         [](Config& c, const std::string& k, const std::string& v) {
             if (!log::parse_level(v, c.diagnostics.log_level)) return bad_value(k, v, "error, warn, info, debug or trace");
             return Status::ok_status();
         }},
        {{"diagnostics.stats_interval", "SECONDS", "how often status/statistics are printed (default 1)"},
         [](Config& c, const std::string& k, const std::string& v) {
             if (!parse_double(v, c.diagnostics.stats_interval_s) || c.diagnostics.stats_interval_s <= 0) {
                 return bad_value(k, v, "seconds > 0");
             }
             return Status::ok_status();
         }},
        {{"diagnostics.progress", "true|false", "live progress and status lines (default true)"},
         [](Config& c, const std::string& k, const std::string& v) {
             if (!parse_bool(v, c.diagnostics.progress)) return bad_value(k, v, "true or false");
             return Status::ok_status();
         }},
        {{"diagnostics.stats_json", "PATH", "append periodic statistics as JSON lines"},
         [](Config& c, const std::string&, const std::string& v) { c.diagnostics.stats_json = v; return Status::ok_status(); }},
        {{"diagnostics.timing_csv", "PATH", "write one CSV row per output frame"},
         [](Config& c, const std::string&, const std::string& v) { c.diagnostics.timing_csv = v; return Status::ok_status(); }},
        {{"diagnostics.report_json", "PATH", "write the final timing report as JSON"},
         [](Config& c, const std::string&, const std::string& v) { c.diagnostics.report_json = v; return Status::ok_status(); }},
        {{"control.stdin", "true|false", "accept typed commands (pause, resume, seek N, stop) (default true)"},
         [](Config& c, const std::string& k, const std::string& v) {
             if (!parse_bool(v, c.read_stdin_commands)) return bad_value(k, v, "true or false");
             return Status::ok_status();
         }},
    };
    return table;
}

}  // namespace

Status apply_setting(Config& config, const std::string& key, const std::string& value) {
    for (const SettingEntry& entry : entries()) {
        if (key == entry.help.key) {
            return entry.setter(config, key, value);
        }
    }
    return Status(StatusCode::InvalidArgument, "unknown setting '" + key + "' (see --help for the list)");
}

const std::vector<SettingHelp>& setting_help() {
    static const std::vector<SettingHelp> help = [] {
        std::vector<SettingHelp> result;
        for (const SettingEntry& entry : entries()) {
            result.push_back(entry.help);
        }
        return result;
    }();
    return help;
}

Status validate(const Config& config) {
    const SourceType type = resolve_source_type(SourceSpec{config.input.type, config.input.path, {}, 0, 0, 0});
    if (config.input.path.empty() && type != SourceType::Push && type != SourceType::Pattern) {
        return Status(StatusCode::InvalidArgument, "no input given: use --input <video|directory|pattern>");
    }
    if (!config.playback.realtime) {
        if (type == SourceType::Push) {
            return Status(StatusCode::InvalidArgument, "fast simulation mode cannot be used with live (push) input");
        }
        const bool bounded = config.playback.on_eof == EofPolicy::Stop || config.playback.duration_s > 0 ||
                             config.playback.max_frames > 0;
        if (!bounded) {
            return Status(StatusCode::InvalidArgument,
                          "fast simulation mode needs an end: use --on-eof stop, --duration or --max-frames");
        }
    }
    return Status::ok_status();
}

const char* output_backend_name(OutputBackend backend) {
    switch (backend) {
        case OutputBackend::V4L2: return "v4l2";
        case OutputBackend::Null: return "null";
        case OutputBackend::File: return "file";
    }
    return "?";
}

const char* buffer_mode_name(BufferMode mode) {
    switch (mode) {
        case BufferMode::Stream: return "stream";
        case BufferMode::Ram:    return "ram";
        case BufferMode::Disk:   return "disk";
    }
    return "?";
}

}  // namespace vcam
