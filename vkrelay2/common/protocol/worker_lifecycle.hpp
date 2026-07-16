// Worker session lifecycle state machine.
//
// The supervisor tracks each worker through an explicit, append-only state
// history. Illegal transitions are rejected rather than silently applied,
// which is what keeps cleanup deterministic when a worker wedges or crashes.
#ifndef VKRELAY2_COMMON_PROTOCOL_WORKER_LIFECYCLE_HPP
#define VKRELAY2_COMMON_PROTOCOL_WORKER_LIFECYCLE_HPP

#include <string>
#include <vector>

namespace vkr::protocol {

enum class WorkerState {
    Spawned,     // process created, no handshake yet
    Handshaking, // connection/handshake in progress
    Running,     // serving its app/session
    Draining,    // app socket EOF or graceful shutdown requested
    Exited,      // clean exit observed
    Crashed,     // abnormal exit / device-lost
    Killed       // terminated by supervisor (timeout or malformed behavior)
};

const char* to_string(WorkerState state);
bool is_terminal(WorkerState state);
bool can_transition(WorkerState from, WorkerState to);

class WorkerLifecycle {
  public:
    WorkerLifecycle() : state_(WorkerState::Spawned), history_{WorkerState::Spawned} {}

    WorkerState state() const { return state_; }
    bool terminal() const { return is_terminal(state_); }
    const std::vector<WorkerState>& history() const { return history_; }

    // Returns false (and leaves state unchanged) if the transition is illegal.
    bool transition(WorkerState to);

  private:
    WorkerState state_;
    std::vector<WorkerState> history_;
};

} // namespace vkr::protocol

#endif // VKRELAY2_COMMON_PROTOCOL_WORKER_LIFECYCLE_HPP
