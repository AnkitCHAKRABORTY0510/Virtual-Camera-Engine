// =============================================================================
// command_line.hpp — turn argv into a Config
//
// Every command-line option is a short name for a config key, e.g.
//     --fps 30          ->  output.fps = 30
//     --on-eof loop     ->  playback.on_eof = loop
// and `--set key=value` sets any key directly. A --config file is applied
// first, then all other options (so the command line wins).
// =============================================================================
#pragma once

#include <string>

#include "vcam/config/config.hpp"

namespace vcam {

struct CommandLine {
    Config config;
    bool show_help = false;
    bool show_version = false;
};

Result<CommandLine> parse_command_line(int argc, char** argv);

std::string usage_text(const char* program);

}  // namespace vcam
