#include "vcam/timing/realtime.hpp"

#include <pthread.h>    // pthread_setschedparam, pthread_setname_np
#include <sched.h>      // SCHED_FIFO
#include <sys/mman.h>   // mlockall
#include <sys/prctl.h>  // prctl, PR_SET_TIMERSLACK
#include <sys/resource.h>  // setpriority (nice value)
#include <sys/syscall.h>   // SYS_gettid
#include <unistd.h>        // syscall

#include <cerrno>
#include <cstring>

namespace vcam {

std::vector<std::string> configure_timing_thread(const RealtimeOptions& options) {
    std::vector<std::string> notes;

    if (options.minimal_timer_slack) {
        // Timer slack lets the kernel delay wake-ups to batch them; 1 ns = none.
        if (prctl(PR_SET_TIMERSLACK, 1UL, 0UL, 0UL, 0UL) != 0) {
            notes.push_back(std::string("timer slack not reduced: ") + std::strerror(errno));
        }
    }

    if (options.fifo_priority > 0) {
        sched_param parameters{};
        parameters.sched_priority = options.fifo_priority;
        // SCHED_FIFO: the thread runs before all normal threads whenever it is
        // ready. Needs CAP_SYS_NICE or an rtprio limit (ulimit -r).
        const int result = pthread_setschedparam(pthread_self(), SCHED_FIFO, &parameters);
        if (result != 0) {
            notes.push_back(std::string("real-time priority not granted (") + std::strerror(result) +
                            "); run with CAP_SYS_NICE or raise 'ulimit -r'");
        }
    }

    if (options.lock_memory) {
        // MCL_CURRENT | MCL_FUTURE: lock all current and future pages in RAM.
        if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
            notes.push_back(std::string("memory not locked (") + std::strerror(errno) +
                            "); raise 'ulimit -l' or run with CAP_IPC_LOCK");
        }
    }
    return notes;
}

std::string lower_current_thread_priority(int nice_value) {
    // On Linux the nice value belongs to each thread: setpriority() with
    // PRIO_PROCESS and the thread id (gettid) changes only this thread.
    const pid_t thread_id = static_cast<pid_t>(syscall(SYS_gettid));
    if (setpriority(PRIO_PROCESS, static_cast<id_t>(thread_id), nice_value) != 0) {
        return std::string("could not lower the decoder thread priority: ") + std::strerror(errno);
    }
    return {};
}

void set_current_thread_name(const char* name) {
    char truncated[16] = {0};  // Linux limit: 15 characters + terminator
    std::strncpy(truncated, name, sizeof(truncated) - 1);
    pthread_setname_np(pthread_self(), truncated);
}

}  // namespace vcam
