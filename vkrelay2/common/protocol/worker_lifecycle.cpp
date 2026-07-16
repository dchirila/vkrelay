#include "common/protocol/worker_lifecycle.hpp"

namespace vkr::protocol {

const char* to_string(WorkerState state) {
    switch (state) {
    case WorkerState::Spawned:
        return "Spawned";
    case WorkerState::Handshaking:
        return "Handshaking";
    case WorkerState::Running:
        return "Running";
    case WorkerState::Draining:
        return "Draining";
    case WorkerState::Exited:
        return "Exited";
    case WorkerState::Crashed:
        return "Crashed";
    case WorkerState::Killed:
        return "Killed";
    }
    return "Spawned";
}

bool is_terminal(WorkerState state) {
    return state == WorkerState::Exited || state == WorkerState::Crashed ||
           state == WorkerState::Killed;
}

bool can_transition(WorkerState from, WorkerState to) {
    // A worker can be killed or crash from any non-terminal state.
    if (is_terminal(from)) {
        return false;
    }
    if (to == WorkerState::Killed || to == WorkerState::Crashed) {
        return true;
    }
    switch (from) {
    case WorkerState::Spawned:
        return to == WorkerState::Handshaking;
    case WorkerState::Handshaking:
        return to == WorkerState::Running;
    case WorkerState::Running:
        return to == WorkerState::Draining || to == WorkerState::Exited;
    case WorkerState::Draining:
        return to == WorkerState::Exited;
    default:
        return false;
    }
}

bool WorkerLifecycle::transition(WorkerState to) {
    if (!can_transition(state_, to)) {
        return false;
    }
    state_ = to;
    history_.push_back(to);
    return true;
}

} // namespace vkr::protocol
