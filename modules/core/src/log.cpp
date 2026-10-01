#include "vcam/core/log.hpp"

#include <pthread.h>  // pthread_getname_np

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <mutex>

#include "vcam/core/clock_time.hpp"

namespace vcam {

namespace {

// std::atomic so any thread can read the level while another changes it.
std::atomic<int> g_level{static_cast<int>(LogLevel::Info)};

// Serialises writes so lines from different threads never interleave.
std::mutex g_write_mutex;

}  // namespace

namespace log {

void set_level(LogLevel level) {
    g_level.store(static_cast<int>(level), std::memory_order_relaxed);
}

LogLevel level() {
    return static_cast<LogLevel>(g_level.load(std::memory_order_relaxed));
}

bool enabled(LogLevel level) {
    return static_cast<int>(level) <= g_level.load(std::memory_order_relaxed);
}

const char* level_name(LogLevel level) {
    switch (level) {
        case LogLevel::Error: return "ERROR";
        case LogLevel::Warn:  return "WARN ";
        case LogLevel::Info:  return "INFO ";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Trace: return "TRACE";
    }
    return "?????";
}

bool parse_level(std::string_view text, LogLevel& out) {
    std::string lower(text);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower == "error") { out = LogLevel::Error; return true; }
    if (lower == "warn" || lower == "warning") { out = LogLevel::Warn; return true; }
    if (lower == "info") { out = LogLevel::Info; return true; }
    if (lower == "debug") { out = LogLevel::Debug; return true; }
    if (lower == "trace") { out = LogLevel::Trace; return true; }
    return false;
}

void write(LogLevel level, const char* module, const std::string& message) {
    const double seconds = static_cast<double>(process_uptime_ns()) / static_cast<double>(kNanosPerSecond);

    // Thread names (set with pthread_setname_np) make multi-threaded logs readable.
    // Only shown at DEBUG/TRACE to keep normal output short.
    char thread_name[16] = {0};
    const bool show_thread = log::level() >= LogLevel::Debug;
    if (show_thread) {
        pthread_getname_np(pthread_self(), thread_name, sizeof(thread_name));
    }

    std::lock_guard<std::mutex> lock(g_write_mutex);
    if (show_thread) {
        std::fprintf(stderr, "[%9.3f] %s [%s] (%s) %s\n", seconds, level_name(level), module, thread_name,
                     message.c_str());
    } else {
        std::fprintf(stderr, "[%9.3f] %s [%s] %s\n", seconds, level_name(level), module, message.c_str());
    }
}

}  // namespace log

bool RateLimiter::allow() {
    const int64_t now = monotonic_now_ns();
    if (!has_allowed_ || now - last_allowed_ns_ >= interval_ns_) {
        has_allowed_ = true;
        last_allowed_ns_ = now;
        return true;
    }
    ++suppressed_;
    return false;
}

uint64_t RateLimiter::take_suppressed() {
    uint64_t count = suppressed_;
    suppressed_ = 0;
    return count;
}

}  // namespace vcam
