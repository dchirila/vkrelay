// vkrelay2 input transport ring -- a bounded, epoch-stamped,
// motion-coalescing event queue.
//
// The worker's WndProc (on the single window thread) ENQUEUES neutral input events per guest
// XID; the sidecar plane (the RPC server's poll_input handler) DRAINS them. It is internally locked
// by a small DEDICATED mutex -- NOT the backend mutex -- so input capture never contends with the
// data/sidecar planes or the registry (the rule). Like the window registry,
// it is pure/common (no Win32, no X), so the mock backend exercises the identical ring headless on
// both platforms (mock == real).
//
// Two correctness rules from the original relay's input lessons, both proven by tests:
//   - MOTION COALESCING (latest-pending-motion-per-XID): at most ONE pending motion per XID is ever
//     in the ring -- a new motion REPLACES the prior pending one for that XID (moved to the back,
//     so the pointer ends at the latest position), so a mouse-move flood collapses instead of
//     backing up the ring. A button/key/wheel carries its own coords (the sidecar warps before it),
//     so dropping the intermediate motions never lands an action at a stale position.
//   - NON-DISPOSABLE PRIORITY (a hard data-structure guarantee):
//     ONLY Motion is disposable. When the ring is full, a disposable Motion is dropped first; a
//     PROTECTED event (button / wheel / key / focus / close / GeometryRequest) is NEVER dropped to
//     make room. If the ring holds no Motion to evict, an incoming Motion is itself dropped (it is
//     disposable -- the next motion supersedes it), while an incoming PROTECTED event is ADMITTED
//     (a bounded soft-cap overrun) and the overflow is RECORDED (`dropped_`, surfaced by drain) --
//     so geometry/lifecycle/close intent is never SILENTLY discarded. A separate absolute ceiling
//     (kMaxInputQueueHardCeiling) bounds a non-draining sidecar; only there is a protected event
//     dropped, always with the diagnostic set. So a motion flood can never starve user intent, and
//     a full ring can never silently swallow a host request.
#ifndef VKRELAY2_COMMON_SIDECAR_INPUT_QUEUE_HPP
#define VKRELAY2_COMMON_SIDECAR_INPUT_QUEUE_HPP

#include "common/sidecar/sidecar_session.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

namespace vkr::sidecar {

// Sidecar input epoch reconciliation (the epoch-split fix). The worker's poll_input gate
// is authoritative: every drained event's epoch equals its xid's CURRENT worker representation
// epoch. The sidecar tracks an epoch per xid (seeded from register/update toplevel-op responses),
// but a DATA-PLANE create_surface that promotes a placeholder to a GPU surface mints a NEW worker
// epoch WITHOUT any sidecar toplevel-op -- so the sidecar's tracked epoch goes stale and (pre-fix)
// every real event was dropped (`epoch != tracked`). Decide per drained event:
//   * event > tracked  -> the worker advanced (the unobserved promotion); ADOPT it then accept.
//   * event < tracked  -> a genuinely STALE in-flight event from before a remap the sidecar already
//                         advanced past; DROP.
//   * event == tracked -> current; ACCEPT.
enum class InputEpochDecision { Accept, AdoptThenAccept, DropStale };

inline InputEpochDecision reconcile_input_epoch(std::uint64_t tracked, std::uint64_t event_epoch) {
    if (event_epoch > tracked) {
        return InputEpochDecision::AdoptThenAccept;
    }
    if (event_epoch < tracked) {
        return InputEpochDecision::DropStale;
    }
    return InputEpochDecision::Accept;
}

class InputQueue {
  public:
    // Push one event for (xid, epoch). Ignored for xid == 0 or an Invalid/out-of-range kind. Stamps
    // a session-monotonic seq (so PollInput's since_seq cursor never re-delivers). A Motion
    // REPLACES the prior PENDING motion for the same xid anywhere in the ring
    // (latest-pending-per-XID coalescing): the old one is removed and the new appended at the back.
    // Overflow: drop the oldest disposable Motion to make room; if none, an incoming Motion
    // is itself dropped (disposable), while an incoming PROTECTED event is admitted (a soft-cap
    // overrun) -- never silently discarded -- up to an absolute hard ceiling. Any overflow is
    // recorded in `dropped`.
    void enqueue(std::uint64_t xid, std::uint64_t epoch, SidecarInputEvent ev) {
        const auto k = static_cast<InputEventKind>(ev.kind);
        if (xid == 0 || k == InputEventKind::Invalid ||
            ev.kind > static_cast<std::uint32_t>(InputEventKind::GeometryRequest)) {
            return;
        }
        std::lock_guard<std::mutex> lk(mu_);
        ev.xid = xid;
        ev.epoch = epoch;
        ev.seq = ++next_seq_;
        if (k == InputEventKind::Motion) {
            // Latest-pending-motion-per-XID: drop the prior pending motion for this xid (at most
            // one exists by this very invariant), then append the new one at the back below.
            // Scanning from the back finds a consecutive-flood's predecessor immediately (the
            // common case).
            for (auto it = events_.end(); it != events_.begin();) {
                --it;
                if (static_cast<InputEventKind>(it->kind) == InputEventKind::Motion &&
                    it->xid == xid) {
                    events_.erase(it);
                    break;
                }
            }
        }
        if (events_.size() >= kMaxInputQueueEvents) {
            if (drop_oldest_motion_locked()) {
                // Made room by evicting a disposable Motion (the common, intended path).
            } else if (k == InputEventKind::Motion) {
                // Incoming is disposable + no Motion to evict -> drop the INCOMING (do not overrun
                // the ring for a disposable event); the next motion supersedes it.
                dropped_ = true;
                return;
            } else {
                // Incoming is PROTECTED + the ring holds no Motion -> NEVER silently drop it. Admit
                // it (a bounded soft-cap overrun) and record the overflow. Only at an absolute hard
                // ceiling (a non-draining sidecar) do we last-resort drop the oldest, still
                // recorded.
                dropped_ = true;
                if (events_.size() >= kMaxInputQueueHardCeiling) {
                    events_.pop_front();
                }
            }
        }
        events_.push_back(ev);
    }

    // Drain events with seq > since_seq, up to kMaxPollInputEvents, into `out` (removing them).
    // Stale already-acked stragglers (seq <= since_seq) are discarded. `out_next_seq` is the
    // highest seq returned, or `since_seq` if none. Returns whether an overflow drop happened since
    // the last drain (and clears that flag). Bounded: at most kMaxPollInputEvents per call.
    bool drain(std::uint64_t since_seq, std::vector<SidecarInputEvent>& out,
               std::uint64_t& out_next_seq) {
        out.clear();
        out_next_seq = since_seq;
        std::lock_guard<std::mutex> lk(mu_);
        while (!events_.empty() && events_.front().seq <= since_seq) {
            events_.pop_front(); // defensive: a re-poll at a higher cursor discards old stragglers
        }
        while (!events_.empty() && out.size() < kMaxPollInputEvents) {
            out_next_seq = events_.front().seq;
            out.push_back(events_.front());
            events_.pop_front();
        }
        const bool was_dropped = dropped_;
        dropped_ = false;
        return was_dropped;
    }

    // Test/diagnostic: current depth (events still in the ring).
    std::size_t size() const {
        std::lock_guard<std::mutex> lk(mu_);
        return events_.size();
    }

  private:
    // Evict the OLDEST disposable Motion anywhere in the ring to make room (caller holds mu_).
    // Returns true iff a Motion was found + dropped. A PROTECTED event is NEVER dropped here -- the
    // caller decides what to do when no Motion exists (drop the incoming if disposable, else
    // admit + diagnose). So a flood can never starve user intent and a host request is never
    // silently swallowed.
    bool drop_oldest_motion_locked() {
        for (auto it = events_.begin(); it != events_.end(); ++it) {
            if (static_cast<InputEventKind>(it->kind) == InputEventKind::Motion) {
                events_.erase(it);
                dropped_ = true;
                return true;
            }
        }
        return false;
    }

    mutable std::mutex mu_;
    std::deque<SidecarInputEvent> events_; // ordered by ascending seq
    std::uint64_t next_seq_ = 0;
    bool dropped_ = false; // an overflow discarded an event since the last drain
};

} // namespace vkr::sidecar

#endif // VKRELAY2_COMMON_SIDECAR_INPUT_QUEUE_HPP
