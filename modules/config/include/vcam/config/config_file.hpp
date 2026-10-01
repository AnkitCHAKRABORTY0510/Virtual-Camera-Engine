// =============================================================================
// config_file.hpp — read a configuration file (a simple subset of YAML)
//
// Supported (enough for configuration, no external YAML library needed):
//
//     # comment
//     input:
//       type: video
//       path: /path/video.mp4        # trailing comments are fine
//     output:
//       fps: 30
//       device_name: "My Camera"     # quotes are optional
//
// i.e. one level of sections, each with "key: value" lines. Keys map to
// apply_setting("section.key", value). Lists, anchors and multi-line values
// are not supported and are reported as errors with the line number.
// =============================================================================
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "vcam/config/config.hpp"

namespace vcam {

struct ConfigEntry {
    std::string key;    // "section.key"
    std::string value;
    int line = 0;
};

// Parses the text; on error the Status message contains the line number.
Result<std::vector<ConfigEntry>> parse_config_text(const std::string& text);

// Reads the file and applies every entry to `config`.
Status load_config_file(const std::string& path, Config& config);

}  // namespace vcam
