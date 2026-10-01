#include "vcam/core/clock_time.hpp"

#include <time.h>  // clock_gettime, CLOCK_MONOTONIC (POSIX)

namespace vcam {

int64_t monotonic_now_ns() {
    timespec now{};
    // clock_gettime fills `now` with seconds + nanoseconds of the given clock.
    // It is a vDSO call on Linux: no system-call overhead.
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<int64_t>(now.tv_sec) * kNanosPerSecond + now.tv_nsec;
}

namespace {
// Initialised when the program is loaded (before main), so log timestamps
// count from program start, not from the first log message.
const int64_t g_process_start_ns = monotonic_now_ns();
}  // namespace

int64_t process_uptime_ns() {
    return monotonic_now_ns() - g_process_start_ns;
}

}  // namespace vcam
