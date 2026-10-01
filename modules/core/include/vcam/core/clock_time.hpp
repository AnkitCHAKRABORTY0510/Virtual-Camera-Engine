// =============================================================================
// clock_time.hpp — monotonic time helpers
//
// CLOCK_MONOTONIC never jumps (unlike wall-clock time, which NTP or the user can
// change), so it is the only clock used for measuring durations and, later,
// for scheduling output frames.
// =============================================================================
#pragma once

#include <cstdint>

namespace vcam {

constexpr int64_t kNanosPerSecond = 1'000'000'000;
constexpr int64_t kNanosPerMilli = 1'000'000;
constexpr int64_t kNanosPerMicro = 1'000;

// Current CLOCK_MONOTONIC time in nanoseconds (arbitrary origin, e.g. boot).
int64_t monotonic_now_ns();

// Monotonic nanoseconds since the first call to this function in the process
// (used for log timestamps).
int64_t process_uptime_ns();

}  // namespace vcam
