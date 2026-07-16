// Bounded supervisor-lifetime cache for immutable display snapshots.
#ifndef VKRELAY2_WINDOWS_SUPERVISOR_DISPLAY_SNAPSHOT_CACHE_HPP
#define VKRELAY2_WINDOWS_SUPERVISOR_DISPLAY_SNAPSHOT_CACHE_HPP

#include "common/control/control_service.hpp"
#include "windows/supervisor/display_layout_probe.hpp"

#include <atomic>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <string>

namespace vkr::supervisor {

class DisplaySnapshotCache final : public control::DisplaySnapshotProvider {
  public:
    using Capture = std::function<DisplayLayoutProbeResult(const std::string& snapshot_id)>;

    static constexpr std::size_t kDefaultCapacity = 8;

    explicit DisplaySnapshotCache(std::string supervisor_session_id,
                                  std::size_t capacity = kDefaultCapacity, Capture capture = {});

    display::DisplayLayoutDecodeResult capture_for_handshake() override;
    bool resolve_copy(const std::string& snapshot_id, display::DisplayLayout& layout,
                      std::string& reason) const override;

    std::size_t size() const;

  private:
    std::string next_snapshot_id();

    std::string supervisor_session_id_;
    std::size_t capacity_;
    Capture capture_;
    std::atomic<std::uint64_t> next_snapshot_{1};
    mutable std::mutex mutex_;
    std::deque<display::DisplayLayout> snapshots_;
};

} // namespace vkr::supervisor

#endif // VKRELAY2_WINDOWS_SUPERVISOR_DISPLAY_SNAPSHOT_CACHE_HPP
