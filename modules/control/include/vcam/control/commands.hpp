// =============================================================================
// commands.hpp — run-time commands: pause, resume, seek, start, stop, stats
//
// Typed in the terminal (one per line) or sent programmatically. Commands
// that affect output timing are delivered to the pacer thread through a
// mailbox it checks once per frame; a lock is taken only when something is
// actually waiting (an atomic flag makes the common "nothing new" case free).
// =============================================================================
#pragma once

#include <atomic>
#include <deque>
#include <mutex>
#include <string>

#include "vcam/core/status.hpp"

namespace vcam {

enum class CommandType { Start, Pause, Resume, Stop, Seek, Stats, Help };

struct Command {
    CommandType type = CommandType::Help;
    double seconds = 0;  // Seek target
};

// "pause", "resume", "seek 120.5", "stop" / "quit", "start", "stats", "help".
Result<Command> parse_command(const std::string& line);
const char* command_help_text();

class CommandMailbox {
public:
    void post(const Command& command);
    // Returns false immediately when nothing is pending.
    bool take(Command& out);

private:
    std::atomic<bool> pending_{false};
    std::mutex mutex_;
    std::deque<Command> queue_;
};

}  // namespace vcam
