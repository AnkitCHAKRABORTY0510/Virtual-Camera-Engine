#include "vcam/control/state_machine.hpp"

namespace vcam {

const char* state_name(EngineState state) {
    switch (state) {
        case EngineState::Init:      return "INIT";
        case EngineState::Loading:   return "LOADING";
        case EngineState::Buffering: return "BUFFERING";
        case EngineState::Ready:     return "READY";
        case EngineState::Streaming: return "STREAMING";
        case EngineState::Paused:    return "PAUSED";
        case EngineState::Eof:       return "EOF";
        case EngineState::Stopping:  return "STOPPING";
        case EngineState::Stopped:   return "STOPPED";
        case EngineState::Error:     return "ERROR";
    }
    return "?";
}

bool is_terminal(EngineState state) {
    return state == EngineState::Stopped || state == EngineState::Error;
}

bool transition_allowed(EngineState from, EngineState to) {
    using S = EngineState;
    if (from == to) {
        return false;
    }
    if (to == S::Error) {
        return !is_terminal(from);  // any running state can fail
    }
    if (to == S::Stopping) {
        return !is_terminal(from) && from != S::Stopping;
    }
    switch (from) {
        case S::Init:      return to == S::Loading;
        case S::Loading:   return to == S::Buffering;
        case S::Buffering: return to == S::Ready;
        case S::Ready:     return to == S::Streaming;
        case S::Streaming: return to == S::Paused || to == S::Eof;
        case S::Paused:    return to == S::Streaming || to == S::Eof;
        case S::Eof:       return to == S::Paused || to == S::Streaming;
        case S::Stopping:  return to == S::Stopped;
        case S::Stopped:
        case S::Error:     return false;
    }
    return false;
}

void StateMachine::set_listener(Listener listener) {
    std::lock_guard<std::mutex> lock(mutex_);
    listener_ = std::move(listener);
}

EngineState StateMachine::current() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

bool StateMachine::transition(EngineState to) {
    Listener listener;
    EngineState from{};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!transition_allowed(state_, to)) {
            return false;
        }
        from = state_;
        state_ = to;
        listener = listener_;
    }
    if (listener) {
        listener(from, to);  // called outside the lock
    }
    return true;
}

}  // namespace vcam
