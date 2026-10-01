// =============================================================================
// state_machine.hpp — the engine's explicit states (docs/ARCHITECTURE.md §8)
//
//   INIT -> LOADING -> BUFFERING -> READY -> STREAMING <-> PAUSED
//                                              |   ^
//                                              v   | (seek back)
//                                             EOF -+
//   any -> STOPPING -> STOPPED          any -> ERROR
//
// All transitions go through transition(), which rejects illegal jumps, so
// the state can never become inconsistent. A listener is told about every
// change (the engine uses it to wake the control loop).
// =============================================================================
#pragma once

#include <functional>
#include <mutex>

namespace vcam {

enum class EngineState { Init, Loading, Buffering, Ready, Streaming, Paused, Eof, Stopping, Stopped, Error };

const char* state_name(EngineState state);  // "STREAMING"
bool is_terminal(EngineState state);         // STOPPED or ERROR
bool transition_allowed(EngineState from, EngineState to);

class StateMachine {
public:
    using Listener = std::function<void(EngineState from, EngineState to)>;

    void set_listener(Listener listener);
    EngineState current() const;

    // Returns false (and changes nothing) if `to` is not allowed from the current state.
    bool transition(EngineState to);

private:
    mutable std::mutex mutex_;
    EngineState state_ = EngineState::Init;
    Listener listener_;
};

}  // namespace vcam
