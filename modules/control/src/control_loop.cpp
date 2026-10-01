#include "vcam/control/control_loop.hpp"

#include <poll.h>
#include <signal.h>
#include <sys/signalfd.h>  // signalfd: receive signals as readable data
#include <sys/timerfd.h>   // timerfd: a periodic timer as a file descriptor
#include <unistd.h>

#include <cstdlib>
#include <cstring>

#include "vcam/core/clock_time.hpp"
#include "vcam/core/text_format.hpp"
#include "vcam/output/v4l2_loopback_camera.hpp"

namespace vcam {

namespace {

constexpr int64_t kTickNs = 50'000'000;  // 50 ms

std::string fps_text(Rational fps) {
    char text[64];
    std::snprintf(text, sizeof(text), "%.3f", to_double(fps));
    std::string value = text;
    // "30.000" -> "30"; keep "29.970"
    while (!value.empty() && value.back() == '0') value.pop_back();
    if (!value.empty() && value.back() == '.') value.pop_back();
    return value;
}

}  // namespace

void block_termination_signals() {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    // pthread_sigmask on the main thread: every thread created afterwards
    // inherits the mask, so these signals reach only our signalfd.
    pthread_sigmask(SIG_BLOCK, &set, nullptr);
    signal(SIGPIPE, SIG_IGN);
}

ControlLoop::ControlLoop(Engine& engine) : engine_(engine), config_(engine.config()) {}

void ControlLoop::end_status_line() {
    if (status_line_open_) {
        std::printf("\n");
        status_line_open_ = false;
    }
}

int ControlLoop::run() {
    start_ns_ = monotonic_now_ns();
    tty_ = isatty(STDOUT_FILENO) != 0;
    if (!config_.diagnostics.stats_json.empty()) {
        stats_json_ = std::fopen(config_.diagnostics.stats_json.c_str(), "a");
    }

    // ---- file descriptors to wait on ----
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    const int signal_fd = signalfd(-1, &signals, SFD_CLOEXEC | SFD_NONBLOCK);

    const int timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    itimerspec interval{};
    interval.it_interval.tv_nsec = kTickNs;
    interval.it_value.tv_nsec = kTickNs;
    timerfd_settime(timer_fd, 0, &interval, nullptr);

    std::printf("Virtual Camera Engine\n---------------------\n");
    std::printf("Input       : %s\n", config_.input.path.empty() ? "(push socket /tmp/vcam.sock)"
                                                                 : config_.input.path.c_str());
    std::printf("Output      : %s%s\n",
                config_.output.backend == OutputBackend::V4L2 ? config_.output.device.c_str()
                                                              : output_backend_name(config_.output.backend),
                config_.playback.realtime ? "" : "  (fast simulation mode)");
    std::fflush(stdout);

    Status started = engine_.start();
    if (!started.ok()) {
        std::printf("[ERROR] %s\n", started.message().c_str());
        engine_.shutdown();
        return engine_.exit_code() != kExitOk ? engine_.exit_code() : kExitConfig;
    }
    on_state_change();

    while (true) {
        pollfd fds[4] = {
            {signal_fd, POLLIN, 0},
            {engine_.event_fd(), POLLIN, 0},
            {timer_fd, POLLIN, 0},
            {(config_.read_stdin_commands && stdin_open_) ? STDIN_FILENO : -1, POLLIN, 0},  // fd -1 is ignored
        };
        if (poll(fds, 4, -1) < 0) {
            continue;  // EINTR
        }
        if (fds[0].revents & POLLIN) {
            signalfd_siginfo info{};
            while (read(signal_fd, &info, sizeof(info)) == sizeof(info)) {
                if (++signals_received_ >= 2) {
                    std::fprintf(stderr, "\nforced exit\n");
                    std::_Exit(130);
                }
                end_status_line();
                std::printf("[STOPPING] signal received, shutting down (press Ctrl+C again to force)\n");
                std::fflush(stdout);
                engine_.request_stop();
            }
        }
        if (fds[1].revents & POLLIN) {
            on_state_change();
        }
        if (fds[2].revents & POLLIN) {
            uint64_t expirations = 0;
            [[maybe_unused]] const ssize_t ignored = read(timer_fd, &expirations, sizeof(expirations));
            on_timer();
        }
        if (fds[3].revents & (POLLIN | POLLHUP)) {
            on_stdin();
        }

        if (engine_.finished() && !engine_.stop_requested()) {
            engine_.request_stop();
        }
        if (engine_.stop_requested() || is_terminal(engine_.state())) {
            break;
        }
    }

    end_status_line();
    engine_.shutdown();
    on_state_change();
    end_status_line();
    print_final_report();

    ::close(signal_fd);
    ::close(timer_fd);
    if (stats_json_) {
        std::fclose(stats_json_);
    }
    int code = engine_.exit_code();
    if (code == kExitOk && engine_.state() == EngineState::Error) {
        code = kExitRuntime;
    }
    return code;
}

void ControlLoop::on_state_change() {
    engine_.clear_event();
    const EngineState state = engine_.state();
    if (state == shown_state_) {
        return;
    }
    const EngineState previous = shown_state_;
    shown_state_ = state;
    const EngineSnapshot snapshot = engine_.snapshot();
    end_status_line();

    switch (state) {
        case EngineState::Loading:
            std::printf("[LOADING]   opening source and checking the output device...\n");
            break;
        case EngineState::Buffering:
            if (snapshot.source_known) {
                const SourceInfo& source = snapshot.source;
                std::printf("            source : %s %dx%d %s", source.codec_name.c_str(), source.width,
                            source.height, source.native_pixel_format.c_str());
                if (source.nominal_fps) std::printf(", %s fps", fps_text(*source.nominal_fps).c_str());
                if (source.frame_count_estimate) std::printf(", ~%llu frames",
                    static_cast<unsigned long long>(*source.frame_count_estimate));
                if (snapshot.duration_s > 0) std::printf(", %s", format_seconds(snapshot.duration_s).c_str());
                std::printf("\n            output : %s @ %s fps -> %s\n", describe(snapshot.output_format).c_str(),
                            fps_text(snapshot.output_fps).c_str(), snapshot.output_description.c_str());
            }
            if (snapshot.live) {
                std::printf("[BUFFERING] waiting for the first frame from the producer...\n");
            } else if (config_.buffer.mode == BufferMode::Stream) {
                std::printf("[BUFFERING] filling the read-ahead window (%.1f s); the rest is decoded while "
                            "streaming\n",
                            config_.buffer.read_ahead_s);
            } else {
                std::printf("[BUFFERING] decoding the whole input into the %s buffer...\n",
                            buffer_mode_name(config_.buffer.mode));
            }
            break;
        case EngineState::Ready:
            if (snapshot.buffer.kind == "stream") {
                std::printf("[READY]     read-ahead window: %llu frames, %s RAM in total (any video length)\n",
                            static_cast<unsigned long long>(snapshot.buffer.capacity_frames),
                            format_bytes(snapshot.buffer.memory_bytes).c_str());
            } else {
                std::printf("[READY]     %llu frames buffered (%s RAM, %s disk)\n",
                            static_cast<unsigned long long>(snapshot.buffer.frames_loaded),
                            format_bytes(snapshot.buffer.memory_bytes).c_str(),
                            format_bytes(snapshot.buffer.disk_bytes).c_str());
            }
            if (config_.playback.start == StartMode::Auto) {
                engine_.start_streaming();  // the state change wakes us again
            } else {
                std::printf("            manual start: type 'start' to begin streaming\n");
            }
            break;
        case EngineState::Streaming:
            if (previous == EngineState::Ready) {
                print_ready(engine_.snapshot());
            } else {
                std::printf("[STREAMING]\n");
            }
            break;
        case EngineState::Paused:
            std::printf("[PAUSED]    %s\n", config_.playback.pause_output == PauseOutput::Hold
                                                ? "holding the current frame (camera stays live)"
                                                : "output stopped");
            break;
        case EngineState::Eof:
            std::printf("[EOF]       end of input (%s)\n", eof_policy_name(config_.playback.on_eof));
            break;
        case EngineState::Stopping:
            std::printf("[STOPPING]\n");
            break;
        case EngineState::Stopped:
            std::printf("[STOPPED]\n");
            break;
        case EngineState::Error:
            std::printf("[ERROR]     %s\n", snapshot.error.c_str());
            break;
        case EngineState::Init:
            break;
    }
    if (!snapshot.last_message.empty()) {
        std::printf("            %s\n", snapshot.last_message.c_str());
    }
    std::fflush(stdout);
}

void ControlLoop::print_ready(const EngineSnapshot& snapshot) {
    std::printf("[STREAMING] Virtual camera is live\n");
    std::printf("            device : %s\n", snapshot.output_description.c_str());
    const char* fourcc_name = pixel_format_name(snapshot.output_format.pixel_format);
    // Show the exact fraction only when it adds information (e.g. 30000/1001).
    const std::string exact = snapshot.output_fps.den == 1 ? "" : " (exactly " + to_string(snapshot.output_fps) + ")";
    std::printf("            format : %s %dx%d @ %s fps%s\n", fourcc_name, snapshot.output_format.width,
                snapshot.output_format.height, fps_text(snapshot.output_fps).c_str(), exact.c_str());
    if (config_.read_stdin_commands) {
        std::printf("            %s\n", command_help_text());
    }
    std::fflush(stdout);
}

void ControlLoop::on_timer() {
    engine_.drain_metrics();
    const EngineState state = engine_.state();
    const int64_t now = monotonic_now_ns();

    if (state == EngineState::Buffering && config_.diagnostics.progress) {
        const EngineSnapshot snapshot = engine_.snapshot();
        if (snapshot.live || snapshot.frames_expected == 0) {
            return;
        }
        const double fraction =
            std::min(1.0, static_cast<double>(snapshot.buffer.frames_loaded) / static_cast<double>(snapshot.frames_expected));
        const int step = static_cast<int>(fraction * 100);
        char line[160];
        std::snprintf(line, sizeof(line), "[BUFFERING] %3d%% | frames %llu / %llu | memory %s", step,
                      static_cast<unsigned long long>(snapshot.buffer.frames_loaded),
                      static_cast<unsigned long long>(snapshot.frames_expected),
                      format_bytes(snapshot.buffer.memory_bytes + snapshot.buffer.disk_bytes).c_str());
        if (tty_) {
            std::printf("\r\x1b[K%s", line);  // \x1b[K clears the rest of the line
            status_line_open_ = true;
        } else if (step / 25 != last_progress_step_) {  // 0, 25, 50, 75, 100 %
            last_progress_step_ = step / 25;
            std::printf("%s\n", line);
        }
        std::fflush(stdout);
        return;
    }

    const bool streaming = state == EngineState::Streaming || state == EngineState::Paused || state == EngineState::Eof;
    if (!streaming) {
        return;
    }
    if (now - last_stats_ns_ < static_cast<int64_t>(config_.diagnostics.stats_interval_s * 1e9)) {
        return;
    }
    last_stats_ns_ = now;
    const EngineSnapshot snapshot = engine_.snapshot();
    if (!snapshot.last_message.empty()) {
        end_status_line();
        std::printf("            %s\n", snapshot.last_message.c_str());
    }
    if (config_.diagnostics.progress) {
        print_status_line(snapshot);
    }
    write_stats_json(snapshot);
}

void ControlLoop::print_status_line(const EngineSnapshot& s) {
    char line[256];
    const TimingReport& t = s.timing;
    const double fps = t.measured_fps > 0 ? t.measured_fps : 0.0;
    if (s.live) {
        std::snprintf(line, sizeof(line),
                      "%s | received %llu | sent %llu | %.2f fps | late %llu | p99 %.2f ms | dropped %llu",
                      state_name(s.state), static_cast<unsigned long long>(s.buffer.frames_loaded),
                      static_cast<unsigned long long>(s.frames_sent), fps,
                      static_cast<unsigned long long>(t.late_frames),
                      static_cast<double>(t.lateness.p99_ns) / 1e6, static_cast<unsigned long long>(t.missed_slots));
    } else {
        std::snprintf(line, sizeof(line),
                      "%s | %s / %s | sent %llu | %.2f fps | late %llu | p99 %.2f ms | dropped %llu | loops %llu",
                      state_name(s.state), format_seconds(s.position_s).c_str(), format_seconds(s.duration_s).c_str(),
                      static_cast<unsigned long long>(s.frames_sent), fps,
                      static_cast<unsigned long long>(t.late_frames), static_cast<double>(t.lateness.p99_ns) / 1e6,
                      static_cast<unsigned long long>(t.missed_slots), static_cast<unsigned long long>(s.loops));
        if (s.buffer.kind == "stream") {
            // How full the decode-ahead window is (low values = decoder struggling).
            const size_t used = std::strlen(line);
            std::snprintf(line + used, sizeof(line) - used, " | ahead %llu/%llu",
                          static_cast<unsigned long long>(s.buffer.frames_loaded),
                          static_cast<unsigned long long>(s.buffer.capacity_frames));
        }
    }
    if (tty_) {
        std::printf("\r\x1b[K%s", line);
        status_line_open_ = true;
    } else {
        std::printf("%s\n", line);
    }
    std::fflush(stdout);
}

void ControlLoop::write_stats_json(const EngineSnapshot& s) {
    if (!stats_json_) {
        return;
    }
    const double elapsed = static_cast<double>(monotonic_now_ns() - start_ns_) / 1e9;
    std::fprintf(stats_json_,
                 "{\"t\":%.3f,\"state\":\"%s\",\"frames_sent\":%llu,\"slots\":%llu,\"position_s\":%.3f,"
                 "\"buffer_frames\":%llu,\"buffer_memory_bytes\":%llu,\"buffer_disk_bytes\":%llu,"
                 "\"decode_errors\":%llu,\"timing\":%s}\n",
                 elapsed, state_name(s.state), static_cast<unsigned long long>(s.frames_sent),
                 static_cast<unsigned long long>(s.slots), s.position_s,
                 static_cast<unsigned long long>(s.buffer.frames_loaded),
                 static_cast<unsigned long long>(s.buffer.memory_bytes),
                 static_cast<unsigned long long>(s.buffer.disk_bytes),
                 static_cast<unsigned long long>(s.source_stats.decode_errors),
                 format_report_json(s.timing).c_str());
    std::fflush(stats_json_);
}

void ControlLoop::on_stdin() {
    char chunk[512];
    const ssize_t n = read(STDIN_FILENO, chunk, sizeof(chunk));
    if (n <= 0) {
        stdin_open_ = false;  // EOF (e.g. stdin is /dev/null): stop watching it
        return;
    }
    stdin_buffer_.append(chunk, static_cast<size_t>(n));
    size_t newline = 0;
    while ((newline = stdin_buffer_.find('\n')) != std::string::npos) {
        const std::string line = stdin_buffer_.substr(0, newline);
        stdin_buffer_.erase(0, newline + 1);
        if (line.find_first_not_of(" \t\r") == std::string::npos) {
            continue;
        }
        end_status_line();
        auto command = parse_command(line);
        if (!command.ok()) {
            std::printf("            %s\n", command.status().message().c_str());
        } else if (command->type == CommandType::Help) {
            std::printf("            %s\n", command_help_text());
        } else if (command->type == CommandType::Stats) {
            std::printf("%s", format_report_text(engine_.timing_report()).c_str());
        } else {
            engine_.post(command.value());
        }
        std::fflush(stdout);
    }
}

void ControlLoop::print_final_report() {
    const EngineSnapshot snapshot = engine_.snapshot();
    if (snapshot.source_known && (snapshot.source_stats.decode_errors || snapshot.source_stats.corrupt_frames ||
                                  snapshot.source_stats.timestamp_repairs)) {
        std::printf("Source problems: %llu decode errors, %llu damaged frames, %llu timestamp repairs\n",
                    static_cast<unsigned long long>(snapshot.source_stats.decode_errors),
                    static_cast<unsigned long long>(snapshot.source_stats.corrupt_frames),
                    static_cast<unsigned long long>(snapshot.source_stats.timestamp_repairs));
    }
    if (snapshot.slots > 0) {
        std::printf("\n%s", format_report_text(snapshot.timing).c_str());
        if (snapshot.metric_records_lost > 0) {
            std::printf("  (%llu timing records were not collected: control thread too slow)\n",
                        static_cast<unsigned long long>(snapshot.metric_records_lost));
        }
    }
    if (!config_.diagnostics.report_json.empty()) {
        if (std::FILE* file = std::fopen(config_.diagnostics.report_json.c_str(), "w")) {
            std::fprintf(file, "%s\n", format_report_json(snapshot.timing).c_str());
            std::fclose(file);
        }
    }
    std::fflush(stdout);
}

}  // namespace vcam
