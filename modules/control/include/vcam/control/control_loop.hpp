// =============================================================================
// control_loop.hpp — the terminal user interface (main thread)
//
// One poll() loop waits on four things at once, so nothing ever blocks:
//   * signalfd   Ctrl+C / SIGTERM  -> clean stop (a second Ctrl+C forces exit)
//   * eventfd    engine state changed -> print it, start streaming when READY
//   * timerfd    every 50 ms: collect timing records; status line every stats interval
//   * stdin      typed commands: pause, resume, seek <s>, stats, start, stop
//
// IMPORTANT: block_termination_signals() must be called in main() BEFORE any
// thread is created, so that SIGINT/SIGTERM are delivered only via signalfd.
// =============================================================================
#pragma once

#include <cstdio>
#include <string>

#include "vcam/control/engine.hpp"

namespace vcam {

// Blocks SIGINT/SIGTERM for the calling thread and all threads created later,
// and ignores SIGPIPE (a disconnected socket must not kill the process).
void block_termination_signals();

class ControlLoop {
public:
    explicit ControlLoop(Engine& engine);

    // Runs until the engine stops; returns the process exit code.
    int run();

private:
    void on_state_change();
    void on_timer();
    void on_stdin();
    void print_status_line(const EngineSnapshot& snapshot);
    void print_ready(const EngineSnapshot& snapshot);
    void print_final_report();
    void write_stats_json(const EngineSnapshot& snapshot);
    void end_status_line();

    Engine& engine_;
    const Config& config_;
    bool tty_ = false;
    bool status_line_open_ = false;
    EngineState shown_state_ = EngineState::Init;
    int64_t last_stats_ns_ = 0;
    int last_progress_step_ = -1;
    int signals_received_ = 0;
    bool stdin_open_ = true;
    std::string stdin_buffer_;
    std::FILE* stats_json_ = nullptr;
    int64_t start_ns_ = 0;
};

}  // namespace vcam
