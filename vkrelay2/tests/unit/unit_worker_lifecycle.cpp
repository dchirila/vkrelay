#include "common/protocol/worker_lifecycle.hpp"
#include "tests/test_assert.hpp"

using vkr::protocol::can_transition;
using vkr::protocol::is_terminal;
using vkr::protocol::WorkerLifecycle;
using vkr::protocol::WorkerState;

namespace {

void test_happy_path() {
    WorkerLifecycle wl;
    VKR_CHECK(wl.state() == WorkerState::Spawned);
    VKR_CHECK(wl.transition(WorkerState::Handshaking));
    VKR_CHECK(wl.transition(WorkerState::Running));
    VKR_CHECK(wl.transition(WorkerState::Draining));
    VKR_CHECK(wl.transition(WorkerState::Exited));
    VKR_CHECK(wl.terminal());
    VKR_CHECK_EQ(wl.history().size(), static_cast<std::size_t>(5));
}

void test_illegal_transitions() {
    WorkerLifecycle wl;
    // Cannot skip from Spawned straight to Running.
    VKR_CHECK(!wl.transition(WorkerState::Running));
    VKR_CHECK(wl.state() == WorkerState::Spawned);
    // History unchanged after a rejected transition.
    VKR_CHECK_EQ(wl.history().size(), static_cast<std::size_t>(1));
}

void test_kill_and_crash_from_anywhere() {
    WorkerLifecycle a;
    VKR_CHECK(a.transition(WorkerState::Killed));
    VKR_CHECK(a.terminal());
    // No transitions out of a terminal state.
    VKR_CHECK(!a.transition(WorkerState::Running));

    WorkerLifecycle b;
    b.transition(WorkerState::Handshaking);
    VKR_CHECK(b.transition(WorkerState::Crashed));
    VKR_CHECK(b.terminal());
}

void test_predicates() {
    VKR_CHECK(is_terminal(WorkerState::Exited));
    VKR_CHECK(is_terminal(WorkerState::Crashed));
    VKR_CHECK(is_terminal(WorkerState::Killed));
    VKR_CHECK(!is_terminal(WorkerState::Running));
    VKR_CHECK(can_transition(WorkerState::Running, WorkerState::Killed));
    VKR_CHECK(!can_transition(WorkerState::Exited, WorkerState::Running));
}

} // namespace

int main() {
    test_happy_path();
    test_illegal_transitions();
    test_kill_and_crash_from_anywhere();
    test_predicates();
    return vkr::test::finish("unit_worker_lifecycle");
}
