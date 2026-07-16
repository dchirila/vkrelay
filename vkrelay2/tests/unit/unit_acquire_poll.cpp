// Unit tests for the abort-aware acquire poll policy. Pure logic: scripted
// try_acquire / is_aborting / now_ns stubs drive every branch of poll_acquire -- the guest timeout
// contract (infinite / zero / finite), abort release within a quantum, and honest-timeout-only.
#include "windows/worker/acquire_poll.hpp"

#include "tests/test_assert.hpp"

#include <cstdint>
#include <vector>

using vkr::worker::AcquirePollOutcome;
using vkr::worker::kInfiniteTimeout;
using vkr::worker::poll_acquire;

namespace {
// Fake VkResult-equivalent codes (values are arbitrary but distinct; only the timeout code is
// special to the helper).
constexpr int kSuccess = 0;
constexpr int kNotReady = 1;
constexpr int kTimeout = 2; // == timeout_result passed to poll_acquire
constexpr int kOutOfDate = -1000001004;

constexpr std::uint64_t kQuantumNs = 50ull * 1000 * 1000; // 50 ms
constexpr std::uint64_t kMs = 1000ull * 1000;             // 1 ms in ns

const auto kNeverAbort = [] { return false; };
const auto kFrozenClock = [] { return static_cast<std::uint64_t>(0); };
} // namespace

int main() {
    // 1. Nonblocking probe (guest timeout 0): exactly one pass-through call at host timeout 0; a
    //    not-ready result is returned verbatim, never looped.
    {
        std::vector<std::uint64_t> host_timeouts;
        auto try_acq = [&](std::uint64_t to) {
            host_timeouts.push_back(to);
            return kNotReady;
        };
        const AcquirePollOutcome o =
            poll_acquire(0, kQuantumNs, kTimeout, try_acq, kNeverAbort, kFrozenClock);
        VKR_CHECK(!o.aborted);
        VKR_CHECK_EQ(o.result, kNotReady);
        VKR_CHECK_EQ(static_cast<int>(host_timeouts.size()), 1);
        VKR_CHECK_EQ(host_timeouts[0], static_cast<std::uint64_t>(0));
    }

    // 2. Infinite wait, image ready after 3 host quanta: each host call uses the full quantum; the
    //    terminal success is returned; no synthesized timeout.
    {
        std::vector<std::uint64_t> host_timeouts;
        int calls = 0;
        auto try_acq = [&](std::uint64_t to) {
            host_timeouts.push_back(to);
            return (++calls < 3) ? kTimeout : kSuccess;
        };
        const AcquirePollOutcome o = poll_acquire(kInfiniteTimeout, kQuantumNs, kTimeout, try_acq,
                                                  kNeverAbort, kFrozenClock);
        VKR_CHECK(!o.aborted);
        VKR_CHECK_EQ(o.result, kSuccess);
        VKR_CHECK_EQ(calls, 3);
        for (const std::uint64_t to : host_timeouts) {
            VKR_CHECK_EQ(to, kQuantumNs); // infinite -> always the full quantum
        }
    }

    // 3. Infinite wait, abort wins after 2 quanta: releases with aborted=true, does not block.
    {
        int abort_checks = 0;
        auto is_aborting = [&] { return (++abort_checks >= 3); }; // false,false,true
        int acq_calls = 0;
        auto try_acq = [&](std::uint64_t) {
            ++acq_calls;
            return kTimeout;
        };
        const AcquirePollOutcome o = poll_acquire(kInfiniteTimeout, kQuantumNs, kTimeout, try_acq,
                                                  is_aborting, kFrozenClock);
        VKR_CHECK(o.aborted);
        VKR_CHECK_EQ(acq_calls, 2); // aborted before the 3rd acquire
    }

    // 4. Infinite wait, immediate terminal (out-of-date): returned at once, one call.
    {
        int acq_calls = 0;
        auto try_acq = [&](std::uint64_t) {
            ++acq_calls;
            return kOutOfDate;
        };
        const AcquirePollOutcome o = poll_acquire(kInfiniteTimeout, kQuantumNs, kTimeout, try_acq,
                                                  kNeverAbort, kFrozenClock);
        VKR_CHECK(!o.aborted);
        VKR_CHECK_EQ(o.result, kOutOfDate);
        VKR_CHECK_EQ(acq_calls, 1);
    }

    // 5. Finite budget (120 ms), image never ready: honest timeout ONLY once the guest budget is
    //    spent; the final host wait clamps to the remaining budget (20 ms), not a full quantum.
    {
        const std::uint64_t start = 1000ull * 1000 * 1000; // arbitrary T0 (1 s)
        // now_ns() call order: start, then once per loop iteration for `elapsed`.
        std::vector<std::uint64_t> clock = {start, start + 0 * kMs, start + 50 * kMs,
                                            start + 100 * kMs, start + 120 * kMs};
        std::size_t tick = 0;
        auto now_ns = [&] { return clock[tick < clock.size() ? tick++ : clock.size() - 1]; };
        std::vector<std::uint64_t> host_timeouts;
        auto try_acq = [&](std::uint64_t to) {
            host_timeouts.push_back(to);
            return kTimeout;
        };
        const AcquirePollOutcome o =
            poll_acquire(120 * kMs, kQuantumNs, kTimeout, try_acq, kNeverAbort, now_ns);
        VKR_CHECK(!o.aborted);
        VKR_CHECK_EQ(o.result, kTimeout); // honest timeout, budget exhausted
        VKR_CHECK_EQ(static_cast<int>(host_timeouts.size()), 3);
        VKR_CHECK_EQ(host_timeouts[0], kQuantumNs); // 50 ms
        VKR_CHECK_EQ(host_timeouts[1], kQuantumNs); // 50 ms
        VKR_CHECK_EQ(host_timeouts[2], 20 * kMs);   // clamp to remaining budget
    }

    // 6. Finite budget, image ready before the deadline: terminal success, no timeout synthesized.
    {
        const std::uint64_t start = 5ull * 1000 * 1000 * 1000;
        std::vector<std::uint64_t> clock = {start, start + 0 * kMs, start + 50 * kMs};
        std::size_t tick = 0;
        auto now_ns = [&] { return clock[tick < clock.size() ? tick++ : clock.size() - 1]; };
        int calls = 0;
        auto try_acq = [&](std::uint64_t) { return (++calls < 2) ? kTimeout : kSuccess; };
        const AcquirePollOutcome o =
            poll_acquire(200 * kMs, kQuantumNs, kTimeout, try_acq, kNeverAbort, now_ns);
        VKR_CHECK(!o.aborted);
        VKR_CHECK_EQ(o.result, kSuccess);
        VKR_CHECK_EQ(calls, 2);
    }

    return vkr::test::finish("unit_acquire_poll");
}
