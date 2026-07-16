// vkrelay2 chrome RECAPTURE scheduler -- keeps a non-GL placeholder window's
// chrome fresh after its first X Expose.
//
// Problem (diagnosed 2026-06-30): a non-GL toolkit toplevel (e.g. OpenSCAD's Welcome dialog) paints
// its content a beat AFTER its first Expose, into the XComposite-redirected backing pixmap, with NO
// further Expose. The sidecar's chrome capture fires only on Expose/map, so it ships ONE frame --
// captured pre-content => BLACK -- and never refreshes (there is no XDamage). A low-rate
// recapture so the late content reaches the worker.
//
// Design (locked): A-fast + C-lite. A short WARM-UP burst (always ship, fast) then
// a STEADY low-rate placeholder poll that ships only when the content HASH changed (or a heartbeat
// elapsed), bounded to placeholder windows. This pure scheduler owns the cadence + ship/stop
// decision; the Linux sidecar driver owns the actual XComposite capture / hash / PaintChrome ship.
// Like the window registry and input ring it is pure/common (no X, no Win32, no clock --
// monotonic-ms in), so unit_sidecar exercises it identically on both platforms.
//
// Stop signal: the PaintChrome RESPONSE already carries `representation`; a non-"placeholder" value
// (the worker promoted the xid to a real surface, or it is gone) means stop recapturing -- no new
// protocol. The worker is also already fail-safe: accept_placeholder_paint REJECTS a paint once the
// xid is surface-backed, so a racing recapture can never corrupt the GL present.
#ifndef VKRELAY2_COMMON_SIDECAR_CHROME_RECAPTURE_HPP
#define VKRELAY2_COMMON_SIDECAR_CHROME_RECAPTURE_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace vkr::sidecar {

// Cadence (documented constants; not env knobs yet). All monotonic milliseconds.
inline constexpr std::uint64_t kChromeWarmupIntervalMs = 150;  // warm-up burst recapture interval
inline constexpr std::uint64_t kChromeWarmupDurationMs = 3000; // warm-up length after each (re)arm
inline constexpr std::uint64_t kChromeSteadyIntervalMs = 300;  // steady placeholder poll interval
inline constexpr std::uint64_t kChromeHeartbeatMs = 3000;      // steady: ship even if unchanged
inline constexpr std::uint64_t kChromeResizeHoldoffMs =
    250; // defer capture after a resize/configure

// XComposite gives a redirected window a NEW backing pixmap whenever it is resized. A named
// pixmap therefore becomes stale on the realized ConfigureNotify edge, even if a coalesced
// resize storm later returns to the same dimensions before the next capture (A -> B -> A). The
// sidecar uses this pure predicate to drop its name immediately; once dropped, later configure
// events leave it invalid until the settled capture names the then-current backing pixmap.
inline bool chrome_named_pixmap_invalidated_by_configure(bool has_named_pixmap,
                                                         std::uint32_t named_w,
                                                         std::uint32_t named_h,
                                                         std::uint32_t configured_w,
                                                         std::uint32_t configured_h) {
    return has_named_pixmap && (named_w != configured_w || named_h != configured_h);
}

// One rectangle from an XShape BOUNDING region, expressed in window-local pixels. Coordinates can
// be negative; width/height use wide common types so the pure helper can validate before clipping.
struct ChromeShapeRect {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

// Define every pixel outside the current XShape BOUNDING region as opaque black. XComposite names
// the complete rectangular backing pixmap, including storage that a shaped client never repaints;
// forwarding those bytes leaks stale pre-resize pixels into the placeholder. The X server returns
// a canonical region (non-overlapping rectangles), so total clipped rectangle area cannot exceed
// the frame. Bound total fill work to one frame even if a malformed reply contains overlaps.
// Returns false without modifying `bgra` on malformed input/allocation failure.
inline bool chrome_mask_outside_shape(unsigned char* bgra, std::size_t len, std::uint32_t w,
                                      std::uint32_t h,
                                      const std::vector<ChromeShapeRect>& rectangles) {
    if (bgra == nullptr || w == 0 || h == 0 || w > std::numeric_limits<std::size_t>::max() / h) {
        return false;
    }
    const std::size_t pixels = static_cast<std::size_t>(w) * h;
    if (pixels > std::numeric_limits<std::size_t>::max() / 4 || len != pixels * 4) {
        return false;
    }
    if (rectangles.size() == 1 && rectangles[0].x == 0 && rectangles[0].y == 0 &&
        rectangles[0].width == w && rectangles[0].height == h) {
        return true; // default/unshaped window: zero allocation and byte-identical output
    }

    std::vector<unsigned char> covered;
    try {
        covered.assign(pixels, 0);
    } catch (...) {
        return false;
    }
    std::size_t covered_area = 0;
    for (const ChromeShapeRect& r : rectangles) {
        const std::int64_t rx1 = static_cast<std::int64_t>(r.x) + r.width;
        const std::int64_t ry1 = static_cast<std::int64_t>(r.y) + r.height;
        const std::int64_t x0 = std::max<std::int64_t>(0, r.x);
        const std::int64_t y0 = std::max<std::int64_t>(0, r.y);
        const std::int64_t x1 = std::min<std::int64_t>(w, rx1);
        const std::int64_t y1 = std::min<std::int64_t>(h, ry1);
        if (x1 <= x0 || y1 <= y0) {
            continue;
        }
        const std::size_t area =
            static_cast<std::size_t>(x1 - x0) * static_cast<std::size_t>(y1 - y0);
        if (area > pixels - covered_area) {
            return false; // malformed overlap or otherwise over-complex region
        }
        covered_area += area;
        for (std::int64_t y = y0; y < y1; ++y) {
            const std::size_t begin =
                static_cast<std::size_t>(y) * w + static_cast<std::size_t>(x0);
            std::fill(covered.begin() + static_cast<std::ptrdiff_t>(begin),
                      covered.begin() + static_cast<std::ptrdiff_t>(begin + (x1 - x0)),
                      static_cast<unsigned char>(1));
        }
    }
    for (std::size_t i = 0; i < pixels; ++i) {
        if (covered[i] == 0) {
            bgra[i * 4 + 0] = 0;
            bgra[i * 4 + 1] = 0;
            bgra[i * 4 + 2] = 0;
            bgra[i * 4 + 3] = 255;
        }
    }
    return true;
}

// FNV-1a 64-bit over the NORMALIZED BGRA frame + its dims/stride/format. Hash the normalized pixels
// (visual_to_bgra output), NOT raw X image bytes -- hashing before normalization risks visual /
// depth / byte-order false differences. Pure, so the driver and the unit test
// agree.
inline std::uint64_t chrome_content_hash(const unsigned char* bgra, std::size_t len,
                                         std::uint32_t w, std::uint32_t h, std::uint32_t stride,
                                         std::uint32_t format) {
    std::uint64_t hsh = 1469598103934665603ull; // FNV-1a offset basis
    auto mix_u64 = [&](std::uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            hsh ^= (v & 0xffull);
            hsh *= 1099511628211ull; // FNV-1a prime
            v >>= 8;
        }
    };
    mix_u64(w);
    mix_u64(h);
    mix_u64(stride);
    mix_u64(format);
    mix_u64(len);
    for (std::size_t i = 0; i < len; ++i) {
        hsh ^= bgra[i];
        hsh *= 1099511628211ull;
    }
    return hsh;
}

// C (black-flash fix): true iff EVERY pixel of the normalized BGRA frame is black (B==G==R==0;
// alpha ignored). An XComposite backing pixmap is an all-zero fill until the toolkit paints into
// it, so a Qt menu / dialog "maps black" and only paints its real content a beat later (with no
// Expose). Shipping + revealing that premature all-black frame is the visible black flash. This is
// the SIGNATURE of "not yet painted" -- deliberately BLACK, not merely uniform: a real popup can
// legitimately be a SOLID non-black colour (the popup canary fills 0x9933CC), which must still
// ship. Short-circuits on the first non-black pixel (real chrome has a border/text immediately). A
// zero-length or non-4-aligned buffer returns false (never suppress on a malformed frame).
inline bool chrome_frame_is_black(const unsigned char* bgra, std::size_t len) {
    if (len == 0 || len % 4 != 0) {
        return false;
    }
    for (std::size_t i = 0; i < len; i += 4) {
        if (bgra[i] != 0 || bgra[i + 1] != 0 || bgra[i + 2] != 0) { // B, G, R (alpha ignored)
            return false;
        }
    }
    return true;
}

enum class ChromeShipDecision { Ship, Suppress };

// Pure per-xid recapture scheduler. Lifecycle the driver drives:
//   track(xid, now)        on (re)arm -- initial map AND restore; RESTARTS the warm-up burst.
//   forget(xid)            on unmap / destroy / unregister.
//   on_resize(xid, now)    on a ConfigureNotify/resize -- holds off capture until geometry settles.
// Per tick: if due(xid, now), record_capture(xid, now), capture+hash, then if
// decide_ship(xid, now, hash)==Ship ship the frame. After a ship the driver calls record_ship(xid,
// now, hash) ONLY when the worker COMMITTED it (PaintChrome response applied=true --
// an applied=false placeholder paint must not update last_shipped, so the next tick re-ships the
// uncommitted pixels), and ALWAYS calls note_representation (surface/none stops recapture). The
// same holds for the unconditional Expose/map ship. record_*/note_* are no-ops for an untracked xid
// (a window the driver never tracked, or one it forgot on teardown). Classified popups ARE
// tracked now (a Qt menu paints its items after map with no further Expose, so the one-shot map
// capture is black); the driver forgets them on unmap/destroy and re-tracks them on a popup remap.
class ChromeRecapturePolicy {
  public:
    void track(std::uint64_t xid, std::uint64_t now_ms) {
        State& s = windows_[xid];
        s.first_seen_ms = now_ms; // (re)arm restarts the warm-up burst
        s.stopped = false;
        // capture/ship history is intentionally PRESERVED across re-arm: a restore that repaints
        // the same pixels is suppressed by the hash (warm-up still always ships during the burst
        // anyway), while a real repaint changes the hash and ships.
    }

    void forget(std::uint64_t xid) { windows_.erase(xid); }

    void on_resize(std::uint64_t xid, std::uint64_t now_ms) {
        const auto it = windows_.find(xid);
        if (it != windows_.end()) {
            it->second.holdoff_until_ms = now_ms + kChromeResizeHoldoffMs;
        }
    }

    // True iff the xid is TRACKED and still within the post-resize holdoff
    // window (now < the holdoff_until on_resize set). The ConfigureNotify driver uses this to SKIP
    // its immediate cap_ship during an active resize -- the steady recapture tick ships once
    // geometry settles (chrome_capture_bgra re-names the backing pixmap on the size change).
    // Deliberately a SEPARATE query, NOT due(): due() also folds the cadence interval
    // (warm-up/steady), so reusing it for the immediate path would suppress legitimate first ships.
    // Untracked xids (a one-shot window, or one already forgotten) return false -> they keep their
    // existing immediate-ship behavior. Pure; unit-tested.
    bool in_resize_holdoff(std::uint64_t xid, std::uint64_t now_ms) const {
        const auto it = windows_.find(xid);
        return it != windows_.end() && now_ms < it->second.holdoff_until_ms;
    }

    // Due iff tracked, not stopped, past any resize holdoff, and the current-phase interval has
    // elapsed since the last capture (or nothing captured yet).
    bool due(std::uint64_t xid, std::uint64_t now_ms) const {
        const auto it = windows_.find(xid);
        if (it == windows_.end()) {
            return false;
        }
        const State& s = it->second;
        if (s.stopped || now_ms < s.holdoff_until_ms) {
            return false;
        }
        if (!s.ever_captured) {
            return true;
        }
        return now_ms - s.last_capture_ms >= interval_for(s, now_ms);
    }

    // Mark a capture happened this tick -- ALWAYS call when due fires (shipped or not), so the
    // cadence throttles captures to the interval instead of every poll tick.
    void record_capture(std::uint64_t xid, std::uint64_t now_ms) {
        const auto it = windows_.find(xid);
        if (it == windows_.end()) {
            return;
        }
        it->second.ever_captured = true;
        it->second.last_capture_ms = now_ms;
    }

    // WARMUP always ships; STEADY ships iff nothing shipped yet, the content changed, or the
    // heartbeat elapsed. Pure read (does not mutate -- call record_ship after an actual ship).
    ChromeShipDecision decide_ship(std::uint64_t xid, std::uint64_t now_ms,
                                   std::uint64_t hash) const {
        const auto it = windows_.find(xid);
        if (it == windows_.end()) {
            return ChromeShipDecision::Suppress;
        }
        const State& s = it->second;
        if (in_warmup(s, now_ms) || !s.ever_shipped || hash != s.last_shipped_hash ||
            now_ms - s.last_ship_ms >= kChromeHeartbeatMs) {
            return ChromeShipDecision::Ship;
        }
        return ChromeShipDecision::Suppress;
    }

    // Record that a frame with `hash` was shipped at now (recapture pass OR an unconditional
    // Expose/map ship), so change-detection + heartbeat track what the worker actually holds.
    void record_ship(std::uint64_t xid, std::uint64_t now_ms, std::uint64_t hash) {
        const auto it = windows_.find(xid);
        if (it == windows_.end()) {
            return;
        }
        State& s = it->second;
        s.ever_captured = true;
        if (s.last_capture_ms < now_ms) {
            s.last_capture_ms = now_ms;
        }
        s.ever_shipped = true;
        s.last_ship_ms = now_ms;
        s.last_shipped_hash = hash;
    }

    // A non-"placeholder" representation (surface/none) means the xid is no longer a chrome
    // placeholder
    // -> stop recapturing it.
    void note_representation(std::uint64_t xid, const std::string& representation) {
        const auto it = windows_.find(xid);
        if (it != windows_.end() && representation != "placeholder") {
            it->second.stopped = true;
        }
    }

    bool tracking(std::uint64_t xid) const { return windows_.count(xid) != 0; }
    bool stopped(std::uint64_t xid) const {
        const auto it = windows_.find(xid);
        return it != windows_.end() && it->second.stopped;
    }

    // C (black-flash fix): true iff the xid is still awaiting its FIRST real (non-black) content --
    // tracked, not stopped, nothing shipped yet, and within the warm-up window. The driver pairs
    // this with chrome_frame_is_black to SUPPRESS the premature all-black map frame (holding the
    // window HIDDEN, since the worker reveals only on a committed first paint) until the toolkit
    // paints. Bounded to warm-up so a window that is genuinely black is never hidden more than
    // kChromeWarmupDurationMs -- after that a black frame ships (reveal), the prior behavior. Once
    // any frame ships, this is false forever for that arm, so steady black frames (a window that
    // legitimately went black) ship normally. Pure; unit-tested.
    bool holding_for_first_content(std::uint64_t xid, std::uint64_t now_ms) const {
        const auto it = windows_.find(xid);
        if (it == windows_.end()) {
            return false;
        }
        const State& s = it->second;
        return !s.stopped && !s.ever_shipped && in_warmup(s, now_ms);
    }

  private:
    struct State {
        std::uint64_t first_seen_ms = 0;
        std::uint64_t last_capture_ms = 0;
        std::uint64_t last_ship_ms = 0;
        std::uint64_t last_shipped_hash = 0;
        std::uint64_t holdoff_until_ms = 0;
        bool ever_captured = false;
        bool ever_shipped = false;
        bool stopped = false;
    };
    static bool in_warmup(const State& s, std::uint64_t now_ms) {
        return now_ms - s.first_seen_ms < kChromeWarmupDurationMs;
    }
    static std::uint64_t interval_for(const State& s, std::uint64_t now_ms) {
        return in_warmup(s, now_ms) ? kChromeWarmupIntervalMs : kChromeSteadyIntervalMs;
    }
    std::map<std::uint64_t, State> windows_;
};

} // namespace vkr::sidecar

#endif // VKRELAY2_COMMON_SIDECAR_CHROME_RECAPTURE_HPP
