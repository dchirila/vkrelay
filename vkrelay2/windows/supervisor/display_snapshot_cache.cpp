#include "windows/supervisor/display_snapshot_cache.hpp"

#include <algorithm>
#include <utility>

namespace vkr::supervisor {

DisplaySnapshotCache::DisplaySnapshotCache(std::string supervisor_session_id, std::size_t capacity,
                                           Capture capture)
    : supervisor_session_id_(std::move(supervisor_session_id)),
      capacity_(std::max<std::size_t>(capacity, 1)), capture_(std::move(capture)) {
    if (!capture_) {
        capture_ = probe_display_layout;
    }
}

std::string DisplaySnapshotCache::next_snapshot_id() {
    const std::uint64_t n = next_snapshot_.fetch_add(1, std::memory_order_relaxed);
    return supervisor_session_id_ + "/display-" + std::to_string(n);
}

display::DisplayLayoutDecodeResult DisplaySnapshotCache::capture_for_handshake() {
    display::DisplayLayoutDecodeResult decoded;
    const std::string expected_id = next_snapshot_id();
    DisplayLayoutProbeResult captured = capture_(expected_id);
    if (!captured.ok) {
        decoded.status = display::LayoutDecodeStatus::Malformed;
        decoded.reason = captured.reason.empty() ? "display topology capture failed"
                                                 : std::move(captured.reason);
        return decoded;
    }
    if (captured.layout.snapshot_id != expected_id) {
        decoded.status = display::LayoutDecodeStatus::Malformed;
        decoded.reason = "display probe returned a snapshot ID other than the injected ID";
        return decoded;
    }
    const display::ValidationResult validation = display::validate_display_layout(captured.layout);
    if (!validation.ok) {
        decoded.status = display::LayoutDecodeStatus::Malformed;
        decoded.reason = "captured display layout is invalid: " + validation.reason;
        return decoded;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshots_.push_back(captured.layout);
        while (snapshots_.size() > capacity_) {
            snapshots_.pop_front();
        }
    }
    decoded.status = display::LayoutDecodeStatus::Valid;
    decoded.layout = std::move(captured.layout);
    return decoded;
}

bool DisplaySnapshotCache::resolve_copy(const std::string& snapshot_id,
                                        display::DisplayLayout& layout, std::string& reason) const {
    const std::string prefix = supervisor_session_id_ + "/display-";
    if (snapshot_id.compare(0, prefix.size(), prefix) != 0) {
        reason = "display snapshot belongs to another supervisor process";
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found =
        std::find_if(snapshots_.begin(), snapshots_.end(), [&](const display::DisplayLayout& item) {
            return item.snapshot_id == snapshot_id;
        });
    if (found == snapshots_.end()) {
        reason = "display snapshot is unknown or expired; re-query once or restart the session";
        return false;
    }
    layout = *found; // session receives an owned immutable copy, never a cache reference
    reason.clear();
    return true;
}

std::size_t DisplaySnapshotCache::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshots_.size();
}

} // namespace vkr::supervisor
