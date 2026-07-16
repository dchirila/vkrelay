// vkrelay2 -- the ICD-side host-visible memory tracker (pure, header-only).
//
// This is the relay's first stateful client-side logic and its hardest piece: the app's
// vkMapMemory pointer is a Linux-side *shadow* buffer here; bytes only reach the worker's real
// VkDeviceMemory when we ship them. The tracker owns the shadow, diffs it against a `last_uploaded`
// mirror in 4 KiB chunks, and produces transactional upload snapshots.
//
// Transactional contract: a snapshot NEVER mutates the mirror; only
// commit()
// -- called after a successful WriteMemoryRanges -- advances it, so a failed upload simply retries
// on the next sweep. vkUnmapMemory is purely local (no RPC): the dirty bytes stay tied to an
// allocation in the candidate set until a *reportable* upload point (vkFlushMappedMemoryRanges or
// vkQueueSubmit, both VkResult) commits or fails them -- the void-call failure surface is removed,
// not retried.
//
// Pure C++ (no Vulkan, no backend): unit-tested directly + dual-platform via unit_icd_memory.
#ifndef VKRELAY2_LINUX_ICD_ICD_MEMORY_H
#define VKRELAY2_LINUX_ICD_ICD_MEMORY_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace vkr::icd_mem {

// Dirty-diff granularity. Correctness is independent of this; it only trades wire bytes for memcmp
// cost on sparse writes (oracle used the same 4 KiB).
inline constexpr std::uint64_t kChunkBytes = 4096;

// Mirrors VK_WHOLE_SIZE (map/flush "to the end of the allocation").
inline constexpr std::uint64_t kWholeSize = ~0ull;

// A byte interval within a single allocation.
struct Range {
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
};

// A flush request range, tagged with its allocation (for snapshot_explicit /
// vkFlushMappedMemoryRanges).
struct MemRange {
    std::uint64_t memory = 0;
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
};

// One allocation's dirty contribution. `ranges` are sorted ascending and disjoint.
struct Upload {
    std::uint64_t memory = 0;
    std::vector<Range> ranges;
};

// The wire payload for a WriteMemoryRanges call AND the commit token: commit() walks
// `uploads`/`ranges` in the same order it was built and scatters `payload` into the mirror, so the
// mirror advances to EXACTLY the acknowledged bytes regardless of any later shadow change.
struct DirtySnapshot {
    std::vector<Upload> uploads;
    std::string payload; // raw bytes, in (upload, range) order
    bool empty() const { return uploads.empty(); }
    std::uint64_t byte_count() const { return static_cast<std::uint64_t>(payload.size()); }
};

// The page pre-filter seam. Given one allocation's shadow span, appends the
// POSSIBLY-WRITTEN byte ranges (relative to `base`, page-granular is fine) to `out` and returns
// true; returning false means "cannot answer" and the caller must diff the WHOLE span (fail
// closed -- more diffing, never less). The contract the sweep relies on: a byte NOT covered by
// `out` was NOT written since the filter's last reset point, and every reset point is
// immediately followed by a full-diff baseline sweep -- so an uncovered byte is guaranteed
// shadow == mirror. False positives (covered but clean) only cost a memcmp. The production
// implementation reads the kernel's soft-dirty PTE bits (icd_softdirty.h); tests inject fakes.
using PageFilter =
    std::function<bool(const std::byte* base, std::uint64_t size, std::vector<Range>& out)>;

// Observability for the sweep (the upload_sweep profile line): eligible = candidate bytes the
// sweep COULD have diffed; diffed = bytes actually memcmp'd after filtering (== eligible when
// no filter applies).
struct SweepIo {
    std::uint64_t eligible_bytes = 0;
    std::uint64_t diffed_bytes = 0;
};

class MappedMemoryTracker {
  public:
    // --- lifecycle (driven by vkAllocateMemory / vkFreeMemory) ---------------------------------
    void on_allocate(std::uint64_t memory, std::uint64_t size, std::uint32_t type_index,
                     bool host_visible, bool host_coherent) {
        Alloc a;
        a.size = size;
        a.type_index = type_index;
        a.host_visible = host_visible;
        a.host_coherent = host_coherent;
        allocs_[memory] = std::move(a);
    }

    // vkFreeMemory implicitly unmaps; the shadow + any pending dirty bytes are discarded (legal: if
    // the bytes were never submitted, the GPU never read them).
    bool on_free(std::uint64_t memory, std::string& err) {
        auto it = allocs_.find(memory);
        if (it == allocs_.end()) {
            err = "free of unknown memory";
            return false;
        }
        allocs_.erase(it);
        return true;
    }

    // --- mapping (WRITE-ONLY: fresh shadow is zero-filled, no download) -------------------
    // Returns a pointer into the persistent shadow buffer for `offset`, or nullptr + err on
    // failure.
    std::byte* on_map(std::uint64_t memory, std::uint64_t offset, std::uint64_t size,
                      std::string& err) {
        auto it = allocs_.find(memory);
        if (it == allocs_.end()) {
            err = "map of unknown memory";
            return nullptr;
        }
        Alloc& a = it->second;
        if (a.is_mapped) {
            err = "memory is already mapped";
            return nullptr;
        }
        if (!a.host_visible || !a.host_coherent) {
            err = "map requires a HOST_VISIBLE | HOST_COHERENT memory type";
            return nullptr;
        }
        if (offset >= a.size) {
            err = "map offset out of bounds";
            return nullptr;
        }
        std::uint64_t effective;
        if (size == kWholeSize) {
            effective = a.size - offset;
        } else {
            if (add_overflows(offset, size) || offset + size > a.size) {
                err = "map range out of bounds";
                return nullptr;
            }
            effective = size;
        }
        if (effective == 0) {
            err = "map range is empty";
            return nullptr;
        }
        ensure_sized(a);
        a.is_mapped = true;
        a.mapped_offset = offset;
        a.mapped_size = effective;
        a.candidate = true; // the app now holds a writable pointer
        return a.shadow.data() + offset;
    }

    // Clears the app pointer only -- NO upload, NO failure path. Dirty bytes remain pending in the
    // candidate set and ship at the next flush/submit.
    bool on_unmap(std::uint64_t memory, std::string& err) {
        auto it = allocs_.find(memory);
        if (it == allocs_.end()) {
            err = "unmap of unknown memory";
            return false;
        }
        if (!it->second.is_mapped) {
            err = "unmap of memory that is not mapped";
            return false;
        }
        it->second.is_mapped = false;
        return true;
    }

    // --- two-phase upload ----------------------------------------------------------------------
    // vkFlushMappedMemoryRanges: snapshot exactly the named ranges (must be currently mapped). Does
    // not mutate the mirror. Fails closed (err set, empty snapshot) on any invalid range.
    DirtySnapshot snapshot_explicit(const std::vector<MemRange>& ranges, std::string& err) {
        DirtySnapshot snap;
        // Group clamped cover intervals by allocation, preserving first-seen order.
        std::vector<std::uint64_t> order;
        std::map<std::uint64_t, std::vector<Range>> cover;
        for (const MemRange& r : ranges) {
            auto it = allocs_.find(r.memory);
            if (it == allocs_.end()) {
                err = "flush of unknown memory";
                return {};
            }
            Alloc& a = it->second;
            if (!a.is_mapped) {
                err = "flush of memory that is not mapped";
                return {};
            }
            // A flush range is relative to the CURRENT mapping, not the whole allocation.
            // VK_WHOLE_SIZE means "from offset to the END OF THE MAPPING"
            // (per VUID-VkMappedMemoryRange-size-00686/01389 -- NOT allocation end, which is the
            // vkMapMemory rule); an explicit size must be > 0 and the whole interval must stay
            // inside the mapping.
            const std::uint64_t off = r.offset;
            const std::uint64_t mapped_end = a.mapped_offset + a.mapped_size;
            std::uint64_t len;
            if (r.size == kWholeSize) {
                if (off < a.mapped_offset || off >= mapped_end) {
                    err = "flush offset is outside the current mapping";
                    return {};
                }
                len = mapped_end - off;
            } else {
                if (r.size == 0) {
                    err = "explicit flush range size must be > 0";
                    return {};
                }
                if (add_overflows(off, r.size) || off < a.mapped_offset ||
                    off + r.size > mapped_end) {
                    err = "flush range is outside the current mapping";
                    return {};
                }
                len = r.size;
            }
            if (cover.find(r.memory) == cover.end()) {
                order.push_back(r.memory);
            }
            cover[r.memory].push_back(Range{off, len});
        }
        for (std::uint64_t mem : order) {
            Alloc& a = allocs_[mem];
            Upload up;
            up.memory = mem;
            collect_dirty(a, merge_cover(cover[mem]), up, snap.payload);
            if (!up.ranges.empty()) {
                snap.uploads.push_back(std::move(up));
            }
        }
        return snap;
    }

    // The vkQueueSubmit coherent sweep: diff EVERY candidate (mapped OR unmapped-but-dirty). Prunes
    // any candidate that is unmapped and diffs clean, preventing accumulation in place.
    //
    // With a `filter`, an allocation that already had its full-diff BASELINE
    // sweep is only diffed over the filter's possibly-written ranges; everything else is
    // guaranteed shadow == mirror by the filter contract above. A filter error (false), a
    // missing filter, or a not-yet-baselined allocation diffs the whole span -- fail closed.
    // PRODUCER CONTRACT: baseline_done flips DURING snapshot
    // construction, but the mirror only advances at commit -- so if ANY frame of a filtered
    // snapshot fails to commit (upload failure/exception), the producer MUST call
    // force_rebaseline() before the next filtered sweep, or unacknowledged bytes whose
    // soft-dirty bits were consumed by the epoch reset become invisible until the next
    // periodic baseline (a silent-stale retry).
    // NOTE the pruning guardrail stays SOUND under filtering: "no dirty ranges found" implies
    // clean only over what was diffed, but an unmapped allocation cannot be written by the app
    // at all, so its filter-admitted set is the complete truth of what changed.
    DirtySnapshot snapshot_pending(const PageFilter& filter = nullptr, SweepIo* io = nullptr) {
        DirtySnapshot snap;
        std::vector<Range> admitted;
        for (auto& kv : allocs_) {
            Alloc& a = kv.second;
            if (!a.candidate) {
                continue;
            }
            Upload up;
            up.memory = kv.first;
            if (!a.shadow.empty()) {
                if (io != nullptr) {
                    io->eligible_bytes += a.size;
                }
                bool full = true;
                if (filter && a.baseline_done) {
                    admitted.clear();
                    if (filter(a.shadow.data(), a.size, admitted)) {
                        // Clamp to the allocation, then merge for the chunk scan.
                        std::vector<Range> cover;
                        for (const Range& r : admitted) {
                            if (r.offset >= a.size || r.size == 0) {
                                continue;
                            }
                            const std::uint64_t end = std::min(
                                add_overflows(r.offset, r.size) ? a.size : r.offset + r.size,
                                a.size);
                            cover.push_back(Range{r.offset, end - r.offset});
                        }
                        cover = merge_cover(std::move(cover));
                        if (io != nullptr) {
                            for (const Range& r : cover) {
                                io->diffed_bytes += r.size;
                            }
                        }
                        collect_dirty(a, cover, up, snap.payload);
                        full = false;
                    }
                }
                if (full) {
                    if (io != nullptr) {
                        io->diffed_bytes += a.size;
                    }
                    collect_dirty(a, std::vector<Range>{Range{0, a.size}}, up, snap.payload);
                    if (filter) {
                        a.baseline_done = true; // full diff done under the filter's epoch
                    }
                }
            }
            if (!up.ranges.empty()) {
                snap.uploads.push_back(std::move(up));
            } else if (!a.is_mapped) {
                a.candidate = false; // clean + unmapped -> can never re-dirty without a new map
            }
        }
        return snap;
    }

    // The caller reset the filter's underlying state (e.g. cleared the
    // kernel's soft-dirty bits) -- every allocation needs a fresh full-diff baseline before the
    // filter may narrow it again. MUST be called immediately after any such reset, BEFORE the
    // next snapshot_pending (the race-free protocol: a reset is only ever followed by a
    // full-diff sweep, so no write can hide between a filter read and a reset).
    void force_rebaseline() {
        for (auto& kv : allocs_) {
            kv.second.baseline_done = false;
        }
    }

    // Advance the mirror to exactly the acknowledged bytes. Call ONLY after a successful
    // WriteMemoryRanges. On a split (multi-frame) sweep, call once per acknowledged frame's
    // snapshot; a never-committed snapshot leaves the mirror untouched so the bytes
    // re-send.
    void commit(const DirtySnapshot& snap) {
        std::size_t cursor = 0;
        for (const Upload& up : snap.uploads) {
            auto it = allocs_.find(up.memory);
            for (const Range& r : up.ranges) {
                if (it != allocs_.end()) {
                    Alloc& a = it->second;
                    if (r.offset + r.size <= a.last_uploaded.size() &&
                        cursor + r.size <= snap.payload.size()) {
                        std::memcpy(a.last_uploaded.data() + r.offset, snap.payload.data() + cursor,
                                    static_cast<std::size_t>(r.size));
                    }
                }
                cursor += static_cast<std::size_t>(r.size);
            }
        }
    }

    // --- queries (predicates + tests) ----------------------------------------------------------
    bool known(std::uint64_t memory) const { return allocs_.find(memory) != allocs_.end(); }
    bool mapped(std::uint64_t memory) const {
        auto it = allocs_.find(memory);
        return it != allocs_.end() && it->second.is_mapped;
    }
    bool host_visible_coherent(std::uint64_t memory) const {
        auto it = allocs_.find(memory);
        return it != allocs_.end() && it->second.host_visible && it->second.host_coherent;
    }
    std::uint64_t allocation_size(std::uint64_t memory) const {
        auto it = allocs_.find(memory);
        return it == allocs_.end() ? 0 : it->second.size;
    }

    // Query-result / GPU-write readback (worker -> ICD): an allocation is safe to overwrite with
    // the worker's current bytes iff it is CLEAN -- shadow == last_uploaded, i.e. no pending local
    // writes the download would clobber. The ICD only ever downloads allocations explicitly flagged
    // as a query-results destination (surgical), so this just guards the clobber case.
    bool readback_clean(std::uint64_t memory) const {
        auto it = allocs_.find(memory);
        return it != allocs_.end() && !it->second.shadow.empty() &&
               it->second.shadow == it->second.last_uploaded;
    }

    // Apply worker-downloaded bytes to a CLEAN allocation's shadow (the app-visible mapped bytes) +
    // advance last_uploaded to match (it stays clean -- the worker holds exactly these bytes). A
    // no-op if the allocation is unknown, became dirty (the app wrote since it was listed), or the
    // length mismatches -- so a race never clobbers a fresh local write.
    void apply_readback(std::uint64_t memory, const std::string& bytes) {
        auto it = allocs_.find(memory);
        if (it == allocs_.end()) {
            return;
        }
        Alloc& a = it->second;
        if (a.shadow.empty() || a.shadow != a.last_uploaded || bytes.size() != a.shadow.size()) {
            return;
        }
        std::memcpy(a.shadow.data(), bytes.data(), bytes.size());
        a.last_uploaded = a.shadow;
    }
    std::size_t candidate_count() const {
        std::size_t n = 0;
        for (const auto& kv : allocs_) {
            if (kv.second.candidate) {
                ++n;
            }
        }
        return n;
    }

  private:
    struct Alloc {
        std::uint64_t size = 0;
        std::uint32_t type_index = 0;
        bool host_visible = false;
        bool host_coherent = false;
        bool is_mapped = false;
        bool candidate = false;     // may hold dirty bytes not yet acknowledged by the worker
        bool baseline_done = false; // full-diffed at least once under the filter epoch
        std::uint64_t mapped_offset = 0;
        std::uint64_t mapped_size = 0;
        std::vector<std::byte> shadow;        // app-visible bytes (sized lazily on first map)
        std::vector<std::byte> last_uploaded; // what the worker is known to hold
    };

    static bool add_overflows(std::uint64_t a, std::uint64_t b) { return a > ~0ull - b; }

    static void ensure_sized(Alloc& a) {
        if (a.shadow.empty() && a.size > 0) {
            // Both buffers start equal (zero) so a freshly mapped, unwritten allocation is clean.
            a.shadow.assign(static_cast<std::size_t>(a.size), std::byte{0});
            a.last_uploaded.assign(static_cast<std::size_t>(a.size), std::byte{0});
        }
    }

    // Merge a set of byte intervals into sorted, non-overlapping cover (so chunk scanning visits
    // each chunk at most once).
    static std::vector<Range> merge_cover(std::vector<Range> in) {
        std::sort(in.begin(), in.end(),
                  [](const Range& x, const Range& y) { return x.offset < y.offset; });
        std::vector<Range> out;
        for (const Range& r : in) {
            if (!out.empty() && r.offset <= out.back().offset + out.back().size) {
                std::uint64_t end =
                    std::max(out.back().offset + out.back().size, r.offset + r.size);
                out.back().size = end - out.back().offset;
            } else {
                out.push_back(r);
            }
        }
        return out;
    }

    // Diff `cover` of allocation `a` in chunk units; append coalesced dirty ranges to `up` and
    // their bytes to `payload`. `cover` must be merged/sorted and clamped to [0, a.size).
    static void collect_dirty(const Alloc& a, const std::vector<Range>& cover, Upload& up,
                              std::string& payload) {
        Range cur{};
        bool have = false;
        std::uint64_t prev_chunk = 0;
        bool first = true;
        for (const Range& iv : cover) {
            std::uint64_t end = std::min(iv.offset + iv.size, a.size);
            std::uint64_t c = (iv.offset / kChunkBytes) * kChunkBytes;
            for (; c < end; c += kChunkBytes) {
                std::uint64_t chunk_end = std::min(c + kChunkBytes, a.size);
                std::uint64_t chunk_idx = c / kChunkBytes;
                bool dirty = std::memcmp(a.shadow.data() + c, a.last_uploaded.data() + c,
                                         static_cast<std::size_t>(chunk_end - c)) != 0;
                bool consecutive = !first && chunk_idx == prev_chunk + 1;
                if (dirty) {
                    if (have && consecutive) {
                        cur.size = chunk_end - cur.offset;
                    } else {
                        if (have) {
                            emit(a, cur, up, payload);
                        }
                        cur = Range{c, chunk_end - c};
                        have = true;
                    }
                } else if (have) {
                    emit(a, cur, up, payload);
                    have = false;
                }
                prev_chunk = chunk_idx;
                first = false;
            }
        }
        if (have) {
            emit(a, cur, up, payload);
        }
    }

    static void emit(const Alloc& a, const Range& r, Upload& up, std::string& payload) {
        up.ranges.push_back(r);
        payload.append(reinterpret_cast<const char*>(a.shadow.data() + r.offset),
                       static_cast<std::size_t>(r.size));
    }

    std::map<std::uint64_t, Alloc> allocs_;
};

// Splits one snapshot into frames whose payload each fits `max_payload` and whose range count each
// fits `max_ranges`, preserving (upload, range) order and slicing a single oversized range across
// frames. Each frame is a self-contained DirtySnapshot (ranges + matching payload), so the producer
// ships one WriteMemoryRanges per frame and commit()s each only after its own ack -- the
// split-per-frame design the ICD producer must actually use. A
// frame never has two uploads for the same memory (the input has one upload per memory and order is
// preserved), so the wire decoder's duplicate-memory check stays satisfied. Pure, so
// unit_icd_memory tests it dual-platform.
inline std::vector<DirtySnapshot>
split_for_upload(const DirtySnapshot& snap, std::size_t max_payload, std::size_t max_ranges) {
    std::vector<DirtySnapshot> frames;
    if (max_payload == 0 || max_ranges == 0) {
        return frames; // defensive: callers pass positive caps
    }
    DirtySnapshot cur;
    std::size_t cur_ranges = 0;
    std::size_t payload_cursor = 0; // position in snap.payload as we walk (upload, range) order
    auto flush_cur = [&]() {
        if (!cur.uploads.empty()) {
            frames.push_back(std::move(cur));
            cur = DirtySnapshot{};
            cur_ranges = 0;
        }
    };
    for (const Upload& up : snap.uploads) {
        for (const Range& r : up.ranges) {
            const char* range_bytes = snap.payload.data() + payload_cursor;
            payload_cursor += static_cast<std::size_t>(r.size);
            std::uint64_t sub_off = r.offset;
            std::uint64_t done = 0;
            while (done < r.size) {
                if (cur.payload.size() >= max_payload || cur_ranges >= max_ranges) {
                    flush_cur();
                }
                const std::uint64_t room = max_payload - cur.payload.size();
                const std::uint64_t take = (r.size - done) < room ? (r.size - done) : room;
                if (cur.uploads.empty() || cur.uploads.back().memory != up.memory) {
                    Upload nu;
                    nu.memory = up.memory;
                    cur.uploads.push_back(std::move(nu));
                }
                cur.uploads.back().ranges.push_back(Range{sub_off, take});
                cur.payload.append(range_bytes + done, static_cast<std::size_t>(take));
                ++cur_ranges;
                sub_off += take;
                done += take;
            }
        }
    }
    flush_cur();
    return frames;
}

} // namespace vkr::icd_mem

#endif // VKRELAY2_LINUX_ICD_ICD_MEMORY_H
