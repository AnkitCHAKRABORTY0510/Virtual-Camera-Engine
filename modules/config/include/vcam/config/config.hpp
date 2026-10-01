// =============================================================================
// config.hpp — every setting of the engine in one place
//
// Values come from three layers, later ones win (docs/ARCHITECTURE.md, §35 of the brief):
//
//     built-in defaults  <  config file (--config file.yaml)  <  command line
//
// Both the config file and the command line go through ONE function,
// apply_setting("section.key", "value"), so the two can never disagree about
// names or validation. `virtual-camera --help` lists every key.
// =============================================================================
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "vcam/core/log.hpp"
#include "vcam/core/pixel_format.hpp"
#include "vcam/core/rational.hpp"
#include "vcam/core/status.hpp"
#include "vcam/resample/playhead.hpp"
#include "vcam/source/source_factory.hpp"

namespace vcam {

enum class OutputBackend { V4L2, Null, File };
// Stream: decode just ahead of playback into a small ring (default, low RAM).
// Ram / Disk: decode the whole input before streaming (exact preload).
enum class BufferMode { Stream, Ram, Disk };
enum class StartMode { Auto, Manual };
enum class PauseOutput { Hold, Stop };

struct InputConfig {
    SourceType type = SourceType::Auto;
    std::string path;                  // file, image directory, "pattern", or socket path (push)
    Rational source_fps{30, 1};        // image sequences and pattern
    int pattern_width = 1280;
    int pattern_height = 720;
    uint64_t pattern_frames = 300;
    int decoder_threads = -1;          // -1 = automatic (2 in stream mode, all cores when preloading)
};

struct OutputConfig {
    OutputBackend backend = OutputBackend::V4L2;
    std::string device = "/dev/video10";
    std::string device_name;           // expected camera name (informational, see docs)
    std::string file_path = "vcam_output.raw";
    std::optional<Rational> fps;       // unset = source's nominal rate
    PixelFormat pixel_format = PixelFormat::YUYV;
};

struct BufferConfig {
    BufferMode mode = BufferMode::Stream;
    double read_ahead_s = 1.0;         // stream mode: seconds of frames decoded ahead
    uint64_t memory_limit = 0;         // bytes; 0 = automatic (RAM: half of available, disk: free space)
    std::string spill_dir = "/var/tmp";
};

struct PlaybackConfig {
    bool realtime = true;              // false = fast simulation (simulated clock)
    EofPolicy on_eof = EofPolicy::Loop;
    SelectionPolicy selection = SelectionPolicy::Hold;
    StartMode start = StartMode::Auto;
    PauseOutput pause_output = PauseOutput::Hold;
    double duration_s = 0;             // stop after this much streaming time (0 = no limit)
    uint64_t max_frames = 0;           // stop after this many output frames (0 = no limit)
};

struct TimingConfig {
    int rt_priority = 0;               // SCHED_FIFO priority (0 = off)
    int spin_us = 0;
    bool lock_memory = false;
    double late_ms = 2.0;              // "late frame" threshold for statistics
};

struct DiagnosticsConfig {
    LogLevel log_level = LogLevel::Info;
    double stats_interval_s = 1.0;
    bool progress = true;              // live progress/status lines in the terminal
    std::string stats_json;            // JSON lines with periodic statistics
    std::string timing_csv;            // one CSV row per output frame
    std::string report_json;           // final timing report as JSON
};

struct Config {
    InputConfig input;
    OutputConfig output;
    BufferConfig buffer;
    PlaybackConfig playback;
    TimingConfig timing;
    DiagnosticsConfig diagnostics;
    bool read_stdin_commands = true;   // pause/resume/seek/stop typed into the terminal
};

// Sets one value by key ("output.fps", "playback.on_eof", ...). Errors name the key.
Status apply_setting(Config& config, const std::string& key, const std::string& value);

// Every accepted key with a one-line description (for --help).
struct SettingHelp {
    const char* key;
    const char* values;
    const char* description;
};
const std::vector<SettingHelp>& setting_help();

// Cross-field checks (e.g. fast mode needs an end condition).
Status validate(const Config& config);

const char* output_backend_name(OutputBackend backend);
const char* buffer_mode_name(BufferMode mode);

}  // namespace vcam
