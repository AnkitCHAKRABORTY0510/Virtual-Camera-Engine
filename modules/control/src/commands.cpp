#include "vcam/control/commands.hpp"

#include <cmath>
#include <cstdlib>
#include <sstream>

namespace vcam {

Result<Command> parse_command(const std::string& line) {
    std::istringstream words(line);
    std::string verb;
    words >> verb;
    Command command;
    if (verb == "pause" || verb == "p") {
        command.type = CommandType::Pause;
    } else if (verb == "resume" || verb == "r" || verb == "play") {
        command.type = CommandType::Resume;
    } else if (verb == "stop" || verb == "quit" || verb == "q" || verb == "exit") {
        command.type = CommandType::Stop;
    } else if (verb == "start") {
        command.type = CommandType::Start;
    } else if (verb == "stats" || verb == "s") {
        command.type = CommandType::Stats;
    } else if (verb == "help" || verb == "h" || verb == "?") {
        command.type = CommandType::Help;
    } else if (verb == "seek") {
        std::string argument;
        words >> argument;
        char* end = nullptr;
        const double seconds = std::strtod(argument.c_str(), &end);
        if (argument.empty() || *end != '\0' || !std::isfinite(seconds)) {
            return Status(StatusCode::InvalidArgument, "usage: seek <seconds>, e.g. seek 120.5");
        }
        command.type = CommandType::Seek;
        command.seconds = seconds;
    } else {
        return Status(StatusCode::InvalidArgument, "unknown command '" + verb + "' (type 'help')");
    }
    return command;
}

const char* command_help_text() {
    return "commands: pause | resume | seek <seconds> | stats | start (manual start) | stop";
}

void CommandMailbox::post(const Command& command) {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.push_back(command);
    pending_.store(true, std::memory_order_release);
}

bool CommandMailbox::take(Command& out) {
    if (!pending_.load(std::memory_order_acquire)) {
        return false;  // fast path: no lock
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.empty()) {
        pending_.store(false, std::memory_order_release);
        return false;
    }
    out = queue_.front();
    queue_.pop_front();
    pending_.store(!queue_.empty(), std::memory_order_release);
    return true;
}

}  // namespace vcam
