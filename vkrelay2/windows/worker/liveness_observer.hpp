// Out-of-band app-connection liveness observer. Runs on its OWN thread so it can
// notice the app died (peer-close) even while the single RPC dispatcher is blocked in a Win32 pump
// call (the WSI-pump wedge). On peer-close it fires `on_peer_closed` once (the session-abort hook,
// which must be thread-safe + nonblocking) so the in-flight blocking acquire releases and teardown
// proceeds; the existing supervisor control-channel-close kill is the hard backstop.
//
// Non-consuming: it uses Connection::wait_peer_closed(), which never steals payload bytes from the
// real reader. Bounded: each wait is at most `quantum_ms`, so `stop` (set when the dispatcher
// returns on a normal EOF) releases the observer within one quantum for a clean join.
//
// Header-only + dependency-light (transport + std) so the observer loop is unit-tested directly
// (integration_worker_teardown) without a real Vulkan backend.
#ifndef VKRELAY2_WINDOWS_WORKER_LIVENESS_OBSERVER_HPP
#define VKRELAY2_WINDOWS_WORKER_LIVENESS_OBSERVER_HPP

#include "common/transport/transport.hpp"

#include <atomic>

namespace vkr::worker {

// Poll quantum for the liveness observer: small enough that a clean join happens promptly, large
// enough not to spin. Also the acquire poll quantum (kept in sync in the real backend).
constexpr int kLivenessQuantumMs = 50;

template <class OnPeerClosed>
void run_liveness_observer(transport::Connection& conn, std::atomic<bool>& stop, int quantum_ms,
                           OnPeerClosed on_peer_closed) {
    while (!stop.load(std::memory_order_relaxed)) {
        if (conn.wait_peer_closed(quantum_ms)) {
            on_peer_closed();
            return;
        }
    }
}

} // namespace vkr::worker

#endif // VKRELAY2_WINDOWS_WORKER_LIVENESS_OBSERVER_HPP
