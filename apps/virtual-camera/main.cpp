// =============================================================================
// virtual-camera — the application: turn a video into a Linux webcam
//
// Kept deliberately thin: parse the configuration, create the Engine, hand
// control to the ControlLoop. Everything else lives in the modules.
//
//   virtual-camera --input video.mp4 --fps 30 --on-eof loop
//   virtual-camera --help
// =============================================================================
#include <cstdio>

#include "vcam/config/command_line.hpp"
#include "vcam/control/control_loop.hpp"
#include "vcam/control/engine.hpp"
#include "vcam/core/log.hpp"

#ifndef VCAM_VERSION
#define VCAM_VERSION "dev"
#endif

int main(int argc, char** argv) {
    // Must happen before any thread exists (see control_loop.hpp).
    vcam::block_termination_signals();

    auto parsed = vcam::parse_command_line(argc, argv);
    if (!parsed.ok()) {
        std::fprintf(stderr, "error: %s\n(see %s --help)\n", parsed.status().message().c_str(), argv[0]);
        return vcam::kExitConfig;
    }
    if (parsed->show_help) {
        std::printf("%s", vcam::usage_text(argv[0]).c_str());
        return vcam::kExitOk;
    }
    if (parsed->show_version) {
        std::printf("virtual-camera %s\n", VCAM_VERSION);
        return vcam::kExitOk;
    }

    const vcam::Config& config = parsed->config;
    vcam::log::set_level(config.diagnostics.log_level);
    vcam::Status valid = vcam::validate(config);
    if (!valid.ok()) {
        std::fprintf(stderr, "error: %s\n(see %s --help)\n", valid.message().c_str(), argv[0]);
        return vcam::kExitConfig;
    }

    vcam::Engine engine(config);
    vcam::ControlLoop control(engine);
    return control.run();
}
