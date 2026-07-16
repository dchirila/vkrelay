// Abort-aware acquire poll policy. Chunks a single guest
// vkAcquireNextImageKHR into short host-timeout calls so the pump thread stays
// interruptible on session teardown, WITHOUT lying to the guest about its
// timeout contract. Pure + dependency-free (no Vulkan, no real clock, no Win32)
// so it is unit-tested directly on both platforms; the worker binds the template
// params to the real host acquire, the session-abort atomic, and a steady clock.
#ifndef VKRELAY2_WINDOWS_WORKER_ACQUIRE_POLL_HPP
#define VKRELAY2_WINDOWS_WORKER_ACQUIRE_POLL_HPP

#include <cstdint>

namespace vkr::worker {

// Outcome of the poll. `aborted` means the session began tearing down before a
// terminal host result arrived -- the caller normally has no peer left to answer,
// so `result` is meaningless then. Otherwise `result` is the terminal host
// VkResult-equivalent (success / suboptimal / out-of-date / device-lost / error,
// or -- only for a finite guest budget -- the timeout code).
struct AcquirePollOutcome {
    int result = 0;
    bool aborted = false;
};

// The guest's "infinite wait" sentinel (VK_WHOLE-style UINT64_MAX). Named here so
// the pure logic never references a Vulkan header.
constexpr std::uint64_t kInfiniteTimeout = ~static_cast<std::uint64_t>(0);

// Preserves the guest timeout EXACTLY while keeping the pump thread interruptible:
//   guest_timeout_ns == UINT64_MAX : loop on `quantum_ns` until a terminal host
//     result or abort -- NEVER synthesize a timeout for an infinite wait.
//   guest_timeout_ns == 0          : one nonblocking host call; return its result
//     verbatim (a not-ready code is legitimate).
//   finite nonzero: deadline via now_ns; each iteration
//     wait min(remaining, quantum_ns); return the timeout code ONLY once the guest
//     budget is spent.
// Any host result other than `timeout_result` is terminal and returned at once.
// `is_aborting()` is polled between iterations (so an infinite wait releases within
// one quantum of teardown). `now_ns()` is a monotonic clock in nanoseconds.
template <class TryAcquire, class IsAborting, class NowNs>
AcquirePollOutcome poll_acquire(std::uint64_t guest_timeout_ns, std::uint64_t quantum_ns,
                                int timeout_result, TryAcquire try_acquire, IsAborting is_aborting,
                                NowNs now_ns) {
    AcquirePollOutcome out;

    // Nonblocking probe: a single pass-through (a not-ready result is legitimate,
    // and we must not loop -- the guest asked for zero wait).
    if (guest_timeout_ns == 0) {
        out.result = try_acquire(0);
        return out;
    }

    const bool infinite = (guest_timeout_ns == kInfiniteTimeout);
    const std::uint64_t start = infinite ? 0 : now_ns();
    for (;;) {
        if (is_aborting()) {
            out.aborted = true;
            return out;
        }
        std::uint64_t host_to = quantum_ns;
        if (!infinite) {
            const std::uint64_t elapsed = now_ns() - start;
            if (elapsed >= guest_timeout_ns) {
                out.result = timeout_result; // guest budget spent -> honest timeout
                return out;
            }
            const std::uint64_t remaining = guest_timeout_ns - elapsed;
            host_to = remaining < quantum_ns ? remaining : quantum_ns;
        }
        const int r = try_acquire(host_to);
        if (r != timeout_result) {
            out.result = r; // terminal host result (success / suboptimal / error / ...)
            return out;
        }
        // host quantum elapsed with no image -> re-check abort + deadline, then retry
    }
}

} // namespace vkr::worker

#endif // VKRELAY2_WINDOWS_WORKER_ACQUIRE_POLL_HPP
