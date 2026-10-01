// =============================================================================
// log.hpp — small structured logger
//
// Output format (one line per message, written atomically to stderr):
//
//   [   1.234] INFO  [source] opened video.mp4
//    ^ seconds since program start (monotonic clock)
//
// Usage:
//   VCAM_INFO("source", "opened " << path << " (" << width << "x" << height << ")");
//
// The macros build the message only if the level is enabled, so a disabled
// VCAM_TRACE costs one integer comparison. Messages use `<<` streaming
// (std::format is not available on GCC 11 / Ubuntu 22.04).
//
// Rule from the architecture: the timing-critical pacer thread never logs;
// it only counts events, and the control thread reports them.
// =============================================================================
#pragma once

#include <atomic>
#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>

namespace vcam {

enum class LogLevel { Error = 0, Warn = 1, Info = 2, Debug = 3, Trace = 4 };

namespace log {

void set_level(LogLevel level);
LogLevel level();

// True when messages of `level` are printed with the current setting.
bool enabled(LogLevel level);

// Writes one finished line. Prefer the VCAM_* macros below.
void write(LogLevel level, const char* module, const std::string& message);

// "error", "warn", "info", "debug", "trace" (case-insensitive).
bool parse_level(std::string_view text, LogLevel& out);
const char* level_name(LogLevel level);

}  // namespace log

// Limits how often a repeated warning is printed (e.g. "corrupted packet").
// allow() returns true at most once per interval and counts what it suppressed,
// so the next printed message can say "(+N similar suppressed)".
class RateLimiter {
public:
    explicit RateLimiter(int64_t interval_ns) : interval_ns_(interval_ns) {}

    // Not thread-safe: use one limiter per thread/component.
    bool allow();
    uint64_t take_suppressed();  // returns and resets the suppressed counter

private:
    int64_t interval_ns_;
    int64_t last_allowed_ns_ = 0;
    bool has_allowed_ = false;
    uint64_t suppressed_ = 0;
};

}  // namespace vcam

// do { ... } while (0) makes the macro behave like one statement, so it is safe
// inside an if/else without braces.
#define VCAM_LOG(level, module, expression)                                 \
    do {                                                                    \
        if (::vcam::log::enabled(level)) {                                  \
            std::ostringstream vcam_log_stream_;                            \
            vcam_log_stream_ << expression;                                 \
            ::vcam::log::write(level, module, vcam_log_stream_.str());      \
        }                                                                   \
    } while (0)

#define VCAM_ERROR(module, expression) VCAM_LOG(::vcam::LogLevel::Error, module, expression)
#define VCAM_WARN(module, expression)  VCAM_LOG(::vcam::LogLevel::Warn, module, expression)
#define VCAM_INFO(module, expression)  VCAM_LOG(::vcam::LogLevel::Info, module, expression)
#define VCAM_DEBUG(module, expression) VCAM_LOG(::vcam::LogLevel::Debug, module, expression)
#define VCAM_TRACE(module, expression) VCAM_LOG(::vcam::LogLevel::Trace, module, expression)
