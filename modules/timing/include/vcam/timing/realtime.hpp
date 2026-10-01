// =============================================================================
// realtime.hpp — make the calling thread wake up as precisely as possible
//
// Linux adds "timer slack" (default 50 µs) to every sleep of a normal thread
// to save power; real-time priority lets the thread pre-empt others; locking
// memory prevents page-fault stalls. All of this is optional and best effort:
// if a setting is not permitted, a note is returned and streaming continues.
// =============================================================================
#pragma once

#include <string>
#include <vector>

namespace vcam {

struct RealtimeOptions {
    bool minimal_timer_slack = true;  // prctl(PR_SET_TIMERSLACK, 1 ns)
    int fifo_priority = 0;            // 1..99 = SCHED_FIFO priority, 0 = normal scheduling
    bool lock_memory = false;         // mlockall(): keep all pages in RAM
};

// Applies the options to the CALLING thread (lock_memory affects the process).
// Returns human-readable notes about what could not be applied.
std::vector<std::string> configure_timing_thread(const RealtimeOptions& options);

// Gives the CALLING thread a lower CPU priority (Linux "nice" value, 0..19;
// higher = more willing to give way). Threads created afterwards by this thread
// (e.g. FFmpeg's decoder threads) inherit it. Used for background decoding so
// the timing-critical pacer always gets the CPU first. Best effort: returns an
// empty string on success, otherwise a note.
std::string lower_current_thread_priority(int nice_value);

// Names the calling thread (visible in `top -H`, gdb and debug logs). Max 15 chars.
void set_current_thread_name(const char* name);

}  // namespace vcam
