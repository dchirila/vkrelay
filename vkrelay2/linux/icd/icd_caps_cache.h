// Guest-side surface-capabilities cache -- a state machine (explicit state, unit-tested
// directly; no map mutation scattered across the Vulkan entrypoints).
//
// zink polls vkGetPhysicalDeviceSurfaceCapabilitiesKHR every frame to detect resize; through the
// relay that is one full round trip per frame. This cache serves the steady-state poll locally,
// with EXPLICIT result-signal invalidation (no TTLs, no side channels):
//
//   - Keyed by (physical_device, surface) (caps are a per-device query; a
//     surface-only key could return one device's caps for another). Invalidation is surface-wide.
//   - Served (and stored) ONLY while the surface has >= 1 live swapchain -- bring-up / initial
//     geometry convergence (placement, clamping, centering, deferred-extent adoption) stays 100%
//     uncached; the cache exists solely for the per-frame poll on an actively-presented surface.
//   - Invalidated on: any non-success acquire/present result touching the surface's swapchain
//     (the worker relays results honestly, so the resize signal already crosses the wire);
//     swapchain create/destroy on the surface (create included -- the recreate flow's first poll
//     re-queries); surface-scoped failure paths; surface destroy (erases all state).
//
// Pure + header-only + templated on the caps value type, so it has zero Vulkan/RPC dependencies
// and is unit-tested on BOTH platforms (unit_icd_subset, the icd_subset.h precedent). The ICD
// instantiates it with vkrpc::GetSurfaceCapabilitiesResponse under g_mu (single-threaded access;
// no locking of its own). Counters make hit/miss/invalidation behavior observable
// (VKRELAY2_ICD_TRACE=1 dumps them at DestroyInstance, so smoke failures explain themselves).
#ifndef VKRELAY2_LINUX_ICD_ICD_CAPS_CACHE_H
#define VKRELAY2_LINUX_ICD_ICD_CAPS_CACHE_H

#include <cstdint>
#include <iterator>
#include <map>
#include <set>
#include <utility>

namespace vkr::icd_caps {

template <typename Caps> class CapsCache {
  public:
    // enabled=false (VKRELAY2_NO_CAPS_CACHE=1) short-circuits every store and lookup -- the
    // correctness-first fallback / debugging escape hatch (the NO_RAW_RECORD pattern). Lifecycle
    // tracking still runs so a mid-run re-enable could never see stale liveness (and surface_of
    // keeps answering for the invalidation hooks, which are then no-ops on an empty cache).
    explicit CapsCache(bool enabled) : enabled_(enabled) {}

    // --- Swapchain / surface lifecycle -------------------------------------------------------
    // A SUCCESSFUL vkCreateSwapchainKHR: the surface becomes (or stays) live AND its cache entry
    // drops (the recreate flow's first poll after create must re-query).
    void on_swapchain_created(std::uint64_t swapchain, std::uint64_t surface) {
        swapchain_surface_[swapchain] = surface;
        live_[surface].insert(swapchain);
        invalidate_surface(surface);
    }
    // vkDestroySwapchainKHR is void: drop local state + invalidate unconditionally (no result to
    // gate on). Unknown handles are a no-op.
    void on_swapchain_destroyed(std::uint64_t swapchain) {
        const auto it = swapchain_surface_.find(swapchain);
        if (it == swapchain_surface_.end()) {
            return;
        }
        const std::uint64_t surface = it->second;
        swapchain_surface_.erase(it);
        const auto lv = live_.find(surface);
        if (lv != live_.end()) {
            lv->second.erase(swapchain);
            if (lv->second.empty()) {
                live_.erase(lv);
            }
        }
        invalidate_surface(surface);
    }
    // vkDestroySurfaceKHR: erase EVERY mapping + cache entry touching the surface.
    void on_surface_destroyed(std::uint64_t surface) {
        for (auto it = swapchain_surface_.begin(); it != swapchain_surface_.end();) {
            it = it->second == surface ? swapchain_surface_.erase(it) : std::next(it);
        }
        live_.erase(surface);
        invalidate_surface(surface);
    }

    // --- Result-signal invalidation ----------------------------------------------------------
    // The surface a swapchain presents to (0 if unknown) -- for per-target present handling.
    std::uint64_t surface_of(std::uint64_t swapchain) const {
        const auto it = swapchain_surface_.find(swapchain);
        return it == swapchain_surface_.end() ? 0 : it->second;
    }
    // Any non-success acquire/present result (or exception) touching this swapchain.
    void invalidate_swapchain(std::uint64_t swapchain) {
        const std::uint64_t surface = surface_of(swapchain);
        if (surface != 0) {
            invalidate_surface(surface);
        }
    }
    // Direct surface invalidation (create-RPC failure, ok=true/result!=success, fault paths).
    // Surface-wide across ALL physical devices.
    void invalidate_surface(std::uint64_t surface) {
        for (auto it = cache_.begin(); it != cache_.end();) {
            if (it->first.second == surface) {
                it = cache_.erase(it);
                ++invalidations_;
            } else {
                ++it;
            }
        }
    }

    // --- Cache ops ---------------------------------------------------------------------------
    // nullptr = miss (query the worker). Serves ONLY while enabled AND the surface is live.
    const Caps* lookup(std::uint64_t physical_device, std::uint64_t surface) {
        if (!enabled_ || !is_live(surface)) {
            ++misses_;
            return nullptr;
        }
        const auto it = cache_.find({physical_device, surface});
        if (it == cache_.end()) {
            ++misses_;
            return nullptr;
        }
        ++hits_;
        return &it->second;
    }
    // Stores ONLY while enabled AND live -- a bring-up-phase store would be dead weight (the
    // swapchain create that starts liveness invalidates anyway), so it is skipped outright.
    void store(std::uint64_t physical_device, std::uint64_t surface, const Caps& caps) {
        if (!enabled_ || !is_live(surface)) {
            return;
        }
        cache_[{physical_device, surface}] = caps;
    }

    // --- Observability -----------------------------------------------------------------------
    bool is_live(std::uint64_t surface) const {
        const auto it = live_.find(surface);
        return it != live_.end() && !it->second.empty();
    }
    std::uint64_t hits() const { return hits_; }
    std::uint64_t misses() const { return misses_; }
    // Counts ERASED ENTRIES (an invalidation event on a surface cached for two physical devices
    // counts 2); an invalidation that finds nothing cached counts 0.
    std::uint64_t invalidations() const { return invalidations_; }

  private:
    bool enabled_;
    std::map<std::uint64_t, std::uint64_t> swapchain_surface_;
    std::map<std::uint64_t, std::set<std::uint64_t>> live_;
    std::map<std::pair<std::uint64_t, std::uint64_t>, Caps> cache_; // (physical_device, surface)
    std::uint64_t hits_ = 0;
    std::uint64_t misses_ = 0;
    std::uint64_t invalidations_ = 0;
};

} // namespace vkr::icd_caps

#endif // VKRELAY2_LINUX_ICD_ICD_CAPS_CACHE_H
