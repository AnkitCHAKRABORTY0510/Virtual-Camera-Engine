// =============================================================================
// clock.hpp — the time source used by the scheduler
//
// The scheduler never calls the OS directly; it asks a Clock. Swapping the
// clock is what separates the two run modes (docs/ARCHITECTURE.md §6.2):
//
//   MonotonicClock   real time: sleeps with clock_nanosleep(TIMER_ABSTIME)
//   SimulatedClock   fast simulation: "sleeping" just moves a counter forward,
//                    so a 10-minute stream runs as fast as the CPU allows,
//                    using exactly the same scheduling and selection code.
// =============================================================================
#pragma once

#include <cstdint>
#include <functional>

namespace vcam {

class Clock {
public:
    virtual ~Clock() = default;
    // Current time in nanoseconds (monotonic: never goes backwards).
    virtual int64_t now_ns() const = 0;
    // Returns at (or shortly after) the ABSOLUTE time `target_ns`.
    // Returns immediately if the target is already in the past.
    virtual void sleep_until_ns(int64_t target_ns) = 0;
    // True for real clocks (used to label reports).
    virtual bool is_realtime() const = 0;
};

class MonotonicClock final : public Clock {
public:
    int64_t now_ns() const override;
    void sleep_until_ns(int64_t target_ns) override;
    bool is_realtime() const override { return true; }
};

class SimulatedClock final : public Clock {
public:
    explicit SimulatedClock(int64_t start_ns = 0) : now_(start_ns) {}

    int64_t now_ns() const override { return now_; }
    void sleep_until_ns(int64_t target_ns) override;
    bool is_realtime() const override { return false; }

    // Moves time forward (e.g. to simulate slow work in a test).
    void advance_ns(int64_t delta_ns) { now_ += delta_ns; }

    // Optional: extra delay added after every sleep, to simulate a late
    // wake-up. Receives the sleep number (0, 1, 2, ...). Used by tests.
    void set_wake_delay(std::function<int64_t(uint64_t sleep_number)> delay) { wake_delay_ = std::move(delay); }

private:
    int64_t now_;
    uint64_t sleeps_ = 0;
    std::function<int64_t(uint64_t)> wake_delay_;
};

}  // namespace vcam
