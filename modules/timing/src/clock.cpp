#include "vcam/timing/clock.hpp"

#include <time.h>  // clock_nanosleep, CLOCK_MONOTONIC, TIMER_ABSTIME

#include <cerrno>

#include "vcam/core/clock_time.hpp"

namespace vcam {

int64_t MonotonicClock::now_ns() const {
    return monotonic_now_ns();
}

void MonotonicClock::sleep_until_ns(int64_t target_ns) {
    timespec target{};
    target.tv_sec = static_cast<time_t>(target_ns / kNanosPerSecond);
    target.tv_nsec = static_cast<long>(target_ns % kNanosPerSecond);
    // clock_nanosleep with TIMER_ABSTIME sleeps until an ABSOLUTE time on the
    // monotonic clock. Unlike "sleep for 33 ms", oversleeping one frame does
    // not shift the next deadline, so errors cannot accumulate (no drift).
    // It returns EINTR if a signal arrives; we simply sleep again.
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &target, nullptr) == EINTR) {
    }
}

void SimulatedClock::sleep_until_ns(int64_t target_ns) {
    if (target_ns > now_) {
        now_ = target_ns;
    }
    if (wake_delay_) {
        now_ += wake_delay_(sleeps_);
    }
    ++sleeps_;
}

}  // namespace vcam
