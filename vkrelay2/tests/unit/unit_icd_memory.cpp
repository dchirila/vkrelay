// Unit tests for the host-visible memory tracker (linux/icd/icd_memory.h).
//
// The tracker is pure C++ (no Vulkan, no backend), so this runs on BOTH platforms unconditionally
// and is where the hardest logic -- the transactional dirty-diff and the coherent-submit sweep
// -- gets its real coverage. Cases: two-phase commit-after-ack,
// partial mappings, VK_WHOLE_SIZE, adjacent-chunk coalescing, disjoint dirty chunks, multiple
// allocations, candidate-only sweep + clean-unmapped pruning, and offset+size overflow / coherence
// rejection.
#include "linux/icd/icd_memory.h"
#include "tests/test_assert.hpp"

#ifdef __linux__
#include "linux/icd/icd_softdirty.h"

#include <sys/mman.h>
#include <unistd.h>
#endif

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace vkr::icd_mem;

namespace {

constexpr std::uint64_t kChunk = kChunkBytes; // 4096

// Write `n` bytes of value `v` at a mapped pointer.
void fill(std::byte* p, std::size_t n, unsigned char v) {
    std::memset(p, v, n);
}

// Total payload bytes across a snapshot.
std::uint64_t total_ranges(const DirtySnapshot& s) {
    std::uint64_t n = 0;
    for (const Upload& u : s.uploads) {
        n += u.ranges.size();
    }
    return n;
}

void test_coherence_gate() {
    std::string err;
    MappedMemoryTracker t;
    t.on_allocate(1, 256, 0, /*host_visible=*/false, /*host_coherent=*/false);
    t.on_allocate(2, 256, 0, /*host_visible=*/true, /*host_coherent=*/false);
    t.on_allocate(3, 256, 0, /*host_visible=*/true, /*host_coherent=*/true);
    err.clear();
    VKR_CHECK(t.on_map(1, 0, kWholeSize, err) == nullptr); // not host-visible
    VKR_CHECK(!err.empty());
    err.clear();
    VKR_CHECK(t.on_map(2, 0, kWholeSize, err) == nullptr); // visible but not coherent
    VKR_CHECK(!err.empty());
    err.clear();
    VKR_CHECK(t.on_map(3, 0, kWholeSize, err) != nullptr); // both -> ok
    VKR_CHECK(err.empty());
    VKR_CHECK(t.host_visible_coherent(3));
    VKR_CHECK(!t.host_visible_coherent(2));
}

void test_map_bounds_and_overflow() {
    std::string err;
    MappedMemoryTracker t;
    t.on_allocate(1, 100, 0, true, true);
    err.clear();
    VKR_CHECK(t.on_map(1, 100, kWholeSize, err) == nullptr); // offset == size
    err.clear();
    VKR_CHECK(t.on_map(1, 50, 60, err) == nullptr); // 50+60 > 100
    err.clear();
    VKR_CHECK(t.on_map(1, ~0ull - 10, 100, err) == nullptr); // offset+size overflow
    err.clear();
    VKR_CHECK(t.on_map(1, 50, 50, err) != nullptr); // exact fit
    VKR_CHECK(err.empty());
    // already mapped
    err.clear();
    VKR_CHECK(t.on_map(1, 0, 10, err) == nullptr);
    VKR_CHECK(!err.empty());
}

void test_two_phase_commit_after_ack() {
    std::string err;
    MappedMemoryTracker t;
    t.on_allocate(1, 2 * kChunk, 0, true, true);
    std::byte* p = t.on_map(1, 0, kWholeSize, err);
    VKR_CHECK(p != nullptr);
    fill(p, 16, 0xAB); // dirty chunk 0 only

    // snapshot does NOT mutate the mirror: two snapshots without a commit are identical +
    // non-empty.
    DirtySnapshot s1 = t.snapshot_pending();
    DirtySnapshot s2 = t.snapshot_pending();
    VKR_CHECK_EQ(s1.uploads.size(), static_cast<std::size_t>(1));
    VKR_CHECK_EQ(s2.uploads.size(), static_cast<std::size_t>(1));
    VKR_CHECK_EQ(s1.uploads[0].ranges.size(), static_cast<std::size_t>(1));
    VKR_CHECK_EQ(s1.uploads[0].ranges[0].offset, static_cast<std::uint64_t>(0));
    VKR_CHECK_EQ(s1.uploads[0].ranges[0].size, kChunk); // whole dirty chunk
    VKR_CHECK_EQ(s1.byte_count(), kChunk);
    VKR_CHECK(static_cast<unsigned char>(s1.payload[0]) == 0xAB);

    // commit advances the mirror; the next sweep is clean.
    t.commit(s1);
    DirtySnapshot s3 = t.snapshot_pending();
    VKR_CHECK(s3.empty());
}

void test_commit_only_after_successful_upload() {
    // case (a): a failed upload (snapshot built, commit skipped) must re-send next sweep.
    std::string err;
    MappedMemoryTracker t;
    t.on_allocate(1, kChunk, 0, true, true);
    std::byte* p = t.on_map(1, 0, kWholeSize, err);
    fill(p, 32, 0x7);
    DirtySnapshot failed = t.snapshot_pending();
    VKR_CHECK(!failed.empty());
    // ... simulate WriteMemoryRanges failure: do NOT commit(failed).
    DirtySnapshot retry = t.snapshot_pending();
    VKR_CHECK(!retry.empty()); // bytes still pending
    VKR_CHECK_EQ(retry.byte_count(), failed.byte_count());
    t.commit(retry); // now the upload succeeded
    VKR_CHECK(t.snapshot_pending().empty());
}

void test_unmap_keeps_pending_then_clean() {
    // unmap is local-only; dirty bytes remain a candidate until committed.
    std::string err;
    MappedMemoryTracker t;
    t.on_allocate(1, kChunk, 0, true, true);
    std::byte* p = t.on_map(1, 0, kWholeSize, err);
    fill(p, 64, 0x5A);
    VKR_CHECK(t.on_unmap(1, err)); // no upload here
    VKR_CHECK(!t.mapped(1));
    DirtySnapshot s = t.snapshot_pending(); // unmapped-but-dirty still swept
    VKR_CHECK(!s.empty());
    VKR_CHECK_EQ(s.uploads[0].memory, static_cast<std::uint64_t>(1));
    t.commit(s);
    DirtySnapshot after = t.snapshot_pending();
    VKR_CHECK(after.empty());
    VKR_CHECK_EQ(t.candidate_count(), static_cast<std::size_t>(0)); // pruned once clean + unmapped
}

void test_successful_upload_then_unmap_sends_nothing() {
    // case (b): committed-then-unmapped allocation contributes nothing to a later sweep.
    std::string err;
    MappedMemoryTracker t;
    t.on_allocate(1, kChunk, 0, true, true);
    std::byte* p = t.on_map(1, 0, kWholeSize, err);
    fill(p, 64, 0x11);
    t.commit(t.snapshot_pending()); // upload succeeded while mapped
    VKR_CHECK(t.on_unmap(1, err));
    DirtySnapshot s = t.snapshot_pending();
    VKR_CHECK(s.empty());
    VKR_CHECK_EQ(t.candidate_count(), static_cast<std::size_t>(0));
}

void test_prune_clean_unmapped_candidate() {
    // Map / no-write / unmap must not accumulate in the candidate set.
    std::string err;
    MappedMemoryTracker t;
    t.on_allocate(1, kChunk, 0, true, true);
    VKR_CHECK(t.on_map(1, 0, kWholeSize, err) != nullptr);
    VKR_CHECK(t.on_unmap(1, err));
    VKR_CHECK_EQ(t.candidate_count(), static_cast<std::size_t>(1));
    DirtySnapshot s = t.snapshot_pending();
    VKR_CHECK(s.empty());
    VKR_CHECK_EQ(t.candidate_count(), static_cast<std::size_t>(0));
}

void test_partial_mapping() {
    std::string err;
    MappedMemoryTracker t;
    t.on_allocate(1, 2 * kChunk, 0, true, true);
    std::byte* p = t.on_map(1, kChunk, kChunk, err); // map the SECOND chunk only
    VKR_CHECK(p != nullptr);
    fill(p, 16, 0x9); // writes into chunk 1
    DirtySnapshot s = t.snapshot_pending();
    VKR_CHECK_EQ(s.uploads.size(), static_cast<std::size_t>(1));
    VKR_CHECK_EQ(s.uploads[0].ranges.size(), static_cast<std::size_t>(1));
    VKR_CHECK_EQ(s.uploads[0].ranges[0].offset, kChunk); // only the mapped/written chunk is dirty
}

void test_whole_size_flush() {
    std::string err;
    MappedMemoryTracker t;
    t.on_allocate(1, kChunk, 0, true, true);
    std::byte* p = t.on_map(1, 0, kWholeSize, err);
    fill(p, 100, 0x3C);
    std::vector<MemRange> ranges{MemRange{1, 0, kWholeSize}};
    DirtySnapshot s = t.snapshot_explicit(ranges, err);
    VKR_CHECK(err.empty());
    VKR_CHECK_EQ(s.uploads.size(), static_cast<std::size_t>(1));
    t.commit(s);
    DirtySnapshot again = t.snapshot_explicit(ranges, err);
    VKR_CHECK(again.empty()); // clean after commit
    VKR_CHECK(t.snapshot_pending().empty());
}

void test_explicit_flush_overflow_rejected() {
    std::string err;
    MappedMemoryTracker t;
    t.on_allocate(1, kChunk, 0, true, true);
    (void) t.on_map(1, 0, kWholeSize, err);
    err.clear();
    DirtySnapshot s = t.snapshot_explicit({MemRange{1, ~0ull - 10, 100}}, err);
    VKR_CHECK(s.empty());
    VKR_CHECK(!err.empty());
    err.clear();
    DirtySnapshot s2 = t.snapshot_explicit({MemRange{99, 0, kWholeSize}}, err); // unknown memory
    VKR_CHECK(s2.empty());
    VKR_CHECK(!err.empty());
}

void test_adjacent_chunk_coalescing() {
    std::string err;
    MappedMemoryTracker t;
    t.on_allocate(1, 3 * kChunk, 0, true, true);
    std::byte* p = t.on_map(1, 0, kWholeSize, err);
    fill(p, 8, 0x1);          // chunk 0
    fill(p + kChunk, 8, 0x2); // chunk 1 (adjacent)
    DirtySnapshot s = t.snapshot_pending();
    VKR_CHECK_EQ(s.uploads.size(), static_cast<std::size_t>(1));
    VKR_CHECK_EQ(s.uploads[0].ranges.size(), static_cast<std::size_t>(1)); // coalesced
    VKR_CHECK_EQ(s.uploads[0].ranges[0].offset, static_cast<std::uint64_t>(0));
    VKR_CHECK_EQ(s.uploads[0].ranges[0].size, 2 * kChunk);
}

void test_disjoint_chunks() {
    std::string err;
    MappedMemoryTracker t;
    t.on_allocate(1, 3 * kChunk, 0, true, true);
    std::byte* p = t.on_map(1, 0, kWholeSize, err);
    fill(p, 8, 0x1);              // chunk 0
    fill(p + 2 * kChunk, 8, 0x3); // chunk 2 (chunk 1 stays clean)
    DirtySnapshot s = t.snapshot_pending();
    VKR_CHECK_EQ(s.uploads.size(), static_cast<std::size_t>(1));
    VKR_CHECK_EQ(s.uploads[0].ranges.size(),
                 static_cast<std::size_t>(2)); // disjoint, not coalesced
    VKR_CHECK_EQ(s.uploads[0].ranges[0].offset, static_cast<std::uint64_t>(0));
    VKR_CHECK_EQ(s.uploads[0].ranges[1].offset, 2 * kChunk);
    VKR_CHECK_EQ(s.byte_count(), 2 * kChunk);
}

void test_multiple_allocations_sweep() {
    std::string err;
    MappedMemoryTracker t;
    t.on_allocate(1, kChunk, 0, true, true);
    t.on_allocate(2, kChunk, 0, true, true);
    t.on_allocate(3, kChunk, 0, true, true); // never mapped -> not a candidate
    std::byte* p1 = t.on_map(1, 0, kWholeSize, err);
    std::byte* p2 = t.on_map(2, 0, kWholeSize, err);
    fill(p1, 8, 0xA);
    fill(p2, 8, 0xB);
    DirtySnapshot s = t.snapshot_pending();
    VKR_CHECK_EQ(s.uploads.size(), static_cast<std::size_t>(2)); // only the two mapped+dirty
    VKR_CHECK_EQ(total_ranges(s), static_cast<std::uint64_t>(2));
}

void test_incremental_diff_after_commit() {
    // After committing chunk 0, a later write to chunk 1 only re-ships chunk 1.
    std::string err;
    MappedMemoryTracker t;
    t.on_allocate(1, 2 * kChunk, 0, true, true);
    std::byte* p = t.on_map(1, 0, kWholeSize, err);
    fill(p, 8, 0x1);
    t.commit(t.snapshot_pending());
    fill(p + kChunk, 8, 0x2); // dirty chunk 1 now
    DirtySnapshot s = t.snapshot_pending();
    VKR_CHECK_EQ(s.uploads.size(), static_cast<std::size_t>(1));
    VKR_CHECK_EQ(s.uploads[0].ranges.size(), static_cast<std::size_t>(1));
    VKR_CHECK_EQ(s.uploads[0].ranges[0].offset, kChunk);
}

void test_free_discards() {
    std::string err;
    MappedMemoryTracker t;
    t.on_allocate(1, kChunk, 0, true, true);
    (void) t.on_map(1, 0, kWholeSize, err);
    VKR_CHECK(t.on_free(1, err)); // implicit unmap + discard
    VKR_CHECK(!t.known(1));
    err.clear();
    VKR_CHECK(!t.on_free(1, err)); // double free -> unknown
    VKR_CHECK(!err.empty());
}

void test_flush_outside_mapping() {
    // A flush range is relative to the CURRENT mapping, and
    // VK_WHOLE_SIZE reaches the END OF THE MAPPING, not the allocation end. Map the MIDDLE chunk of
    // a 3-chunk allocation so mapping-end (2*kChunk) differs from allocation-end (3*kChunk).
    std::string err;
    MappedMemoryTracker t;
    t.on_allocate(1, 3 * kChunk, 0, true, true);
    std::byte* p = t.on_map(1, kChunk, kChunk, err); // map [kChunk, 2*kChunk)
    VKR_CHECK(p != nullptr);
    fill(p, 16, 0x1);
    // Flushing the FIRST (unmapped) chunk is rejected.
    DirtySnapshot s = t.snapshot_explicit({MemRange{1, 0, kChunk}}, err);
    VKR_CHECK(s.empty() && !err.empty());
    // Flushing the mapped chunk is fine.
    err.clear();
    DirtySnapshot s2 = t.snapshot_explicit({MemRange{1, kChunk, kChunk}}, err);
    VKR_CHECK(err.empty() && !s2.empty());
    // Flushing the THIRD chunk (past the mapping, still inside the allocation) is rejected.
    err.clear();
    DirtySnapshot s3 = t.snapshot_explicit({MemRange{1, 2 * kChunk, kChunk}}, err);
    VKR_CHECK(s3.empty() && !err.empty());
    // VK_WHOLE_SIZE from offset 0 starts before the mapping -> rejected.
    err.clear();
    DirtySnapshot s4 = t.snapshot_explicit({MemRange{1, 0, kWholeSize}}, err);
    VKR_CHECK(s4.empty() && !err.empty());
    // VK_WHOLE_SIZE from the mapped offset reaches mapping-end (kChunk long), NOT allocation-end.
    err.clear();
    DirtySnapshot s5 = t.snapshot_explicit({MemRange{1, kChunk, kWholeSize}}, err);
    VKR_CHECK(err.empty() && !s5.empty());
    VKR_CHECK_EQ(s5.uploads[0].ranges[0].offset, kChunk);
    VKR_CHECK_EQ(s5.uploads[0].ranges[0].size, kChunk); // mapping-end - offset, not 2*kChunk
    // An explicit (non-WHOLE_SIZE) zero-size flush range is rejected, not a silent no-op.
    err.clear();
    DirtySnapshot s6 = t.snapshot_explicit({MemRange{1, kChunk, 0}}, err);
    VKR_CHECK(s6.empty() && !err.empty());
}

void test_split_for_upload() {
    // The producer splits an oversized snapshot into cap-fitting frames, in
    // order.
    std::string err;
    MappedMemoryTracker t;
    t.on_allocate(1, 4 * kChunk, 0, true, true);
    std::byte* p = t.on_map(1, 0, kWholeSize, err);
    fill(p, 4 * kChunk, 0xCD); // dirty all 4 chunks -> one coalesced range of 16384 bytes
    const DirtySnapshot snap = t.snapshot_pending();
    VKR_CHECK_EQ(snap.uploads.size(), static_cast<std::size_t>(1));
    VKR_CHECK_EQ(snap.byte_count(), 4 * kChunk);

    // Payload-cap split: one 16384-byte range -> four 4096-byte frames, offsets contiguous, in
    // order.
    const auto frames = split_for_upload(snap, /*max_payload=*/kChunk, /*max_ranges=*/100);
    VKR_CHECK_EQ(frames.size(), static_cast<std::size_t>(4));
    std::string reassembled;
    std::uint64_t expect_off = 0;
    for (const auto& f : frames) {
        VKR_CHECK(f.payload.size() <= kChunk);
        for (const auto& u : f.uploads) {
            for (const auto& r : u.ranges) {
                VKR_CHECK_EQ(r.offset, expect_off);
                expect_off += r.size;
            }
        }
        reassembled += f.payload;
    }
    VKR_CHECK(reassembled == snap.payload);
    // Committing every frame in order leaves the allocation clean.
    for (const auto& f : frames) {
        t.commit(f);
    }
    VKR_CHECK(t.snapshot_pending().empty());

    // Range-count-cap split: a fresh snapshot of two disjoint chunks, max_ranges 1 -> two frames.
    MappedMemoryTracker t2;
    t2.on_allocate(1, 3 * kChunk, 0, true, true);
    std::byte* q = t2.on_map(1, 0, kWholeSize, err);
    fill(q, 8, 0x1);              // chunk 0
    fill(q + 2 * kChunk, 8, 0x2); // chunk 2 (chunk 1 clean) -> two disjoint ranges
    const DirtySnapshot s2 = t2.snapshot_pending();
    VKR_CHECK_EQ(s2.uploads[0].ranges.size(), static_cast<std::size_t>(2));
    const auto fr2 = split_for_upload(s2, /*max_payload=*/1 << 20, /*max_ranges=*/1);
    VKR_CHECK_EQ(fr2.size(), static_cast<std::size_t>(2)); // one range per frame
}

void test_split_partial_commit() {
    // Guardrail: committing only some frames leaves the rest pending (re-sent).
    std::string err;
    MappedMemoryTracker t;
    t.on_allocate(1, 4 * kChunk, 0, true, true);
    std::byte* p = t.on_map(1, 0, kWholeSize, err);
    fill(p, 4 * kChunk, 0xEE);
    const DirtySnapshot snap = t.snapshot_pending();
    const auto frames = split_for_upload(snap, kChunk, 100);
    VKR_CHECK_EQ(frames.size(), static_cast<std::size_t>(4));
    t.commit(frames[0]); // only the first 4096 bytes acked
    const DirtySnapshot again = t.snapshot_pending();
    VKR_CHECK_EQ(again.byte_count(), 3 * kChunk);            // the rest still dirty
    VKR_CHECK_EQ(again.uploads[0].ranges[0].offset, kChunk); // starts at 4096
}

// --- PageFilter seam (soft-dirty pre-filter) ----------------------------------
// The seam's CONTRACT, pinned with fakes: the filter is TRUSTED once an allocation has had its
// full-diff baseline sweep -- chunks outside the admitted ranges are skipped without a memcmp.
// (The production filter is the kernel's soft-dirty bits, which cannot miss a write; a fake
// that lies demonstrates the trust, which is exactly what these tests document.)
void test_filter_baseline_then_narrow() {
    std::string err;
    MappedMemoryTracker t;
    t.on_allocate(1, 4 * kChunk, 0, true, true);
    std::byte* p = t.on_map(1, 0, kWholeSize, err);
    fill(p, 16, 0x11); // chunk 0 dirty

    int filter_calls = 0;
    PageFilter admit_nothing = [&](const std::byte*, std::uint64_t, std::vector<Range>&) {
        ++filter_calls;
        return true; // "no pages written" (a lie here -- the seam must trust it post-baseline)
    };

    // Sweep 1 under a filter: NOT yet baselined -> the filter is ignored, full diff ships.
    SweepIo io1;
    DirtySnapshot s1 = t.snapshot_pending(admit_nothing, &io1);
    VKR_CHECK_EQ(filter_calls, 0); // baseline never consults the filter
    VKR_CHECK_EQ(s1.byte_count(), kChunk);
    VKR_CHECK_EQ(io1.eligible_bytes, 4 * kChunk);
    VKR_CHECK_EQ(io1.diffed_bytes, 4 * kChunk); // full diff
    t.commit(s1);

    // Sweep 2: baselined -> the filter narrows. New dirty bytes exist, but the filter admits
    // nothing, so NOTHING is diffed or shipped (trust, pinned).
    fill(p + kChunk, 16, 0x22);
    SweepIo io2;
    DirtySnapshot s2 = t.snapshot_pending(admit_nothing, &io2);
    VKR_CHECK_EQ(filter_calls, 1);
    VKR_CHECK(s2.empty());
    VKR_CHECK_EQ(io2.diffed_bytes, static_cast<std::uint64_t>(0));

    // Sweep 3: a filter admitting exactly the written page ships exactly that chunk.
    PageFilter admit_chunk1 = [](const std::byte*, std::uint64_t, std::vector<Range>& out) {
        out.push_back(Range{kChunk, kChunk});
        return true;
    };
    SweepIo io3;
    DirtySnapshot s3 = t.snapshot_pending(admit_chunk1, &io3);
    VKR_CHECK_EQ(s3.byte_count(), kChunk);
    VKR_CHECK_EQ(s3.uploads[0].ranges[0].offset, kChunk);
    VKR_CHECK_EQ(io3.diffed_bytes, kChunk);
    t.commit(s3);

    // An admit-everything filter behaves exactly like the full diff (clean -> empty).
    PageFilter admit_all = [](const std::byte*, std::uint64_t size, std::vector<Range>& out) {
        out.push_back(Range{0, size});
        return true;
    };
    SweepIo io4;
    VKR_CHECK(t.snapshot_pending(admit_all, &io4).empty());
    VKR_CHECK_EQ(io4.diffed_bytes, 4 * kChunk);
}

void test_filter_error_and_rebaseline() {
    std::string err;
    MappedMemoryTracker t;
    t.on_allocate(1, 2 * kChunk, 0, true, true);
    std::byte* p = t.on_map(1, 0, kWholeSize, err);
    fill(p, 8, 0x33);
    PageFilter admit_nothing = [](const std::byte*, std::uint64_t, std::vector<Range>&) {
        return true;
    };
    t.commit(t.snapshot_pending(admit_nothing, nullptr)); // baseline + ack

    // A filter that CANNOT answer (returns false) forces the full diff -- dirty bytes ship.
    fill(p, 8, 0x44);
    PageFilter broken = [](const std::byte*, std::uint64_t, std::vector<Range>&) { return false; };
    SweepIo io;
    DirtySnapshot s = t.snapshot_pending(broken, &io);
    VKR_CHECK_EQ(s.byte_count(), kChunk);
    VKR_CHECK_EQ(io.diffed_bytes, 2 * kChunk); // whole span diffed
    t.commit(s);

    // force_rebaseline: the next sweep full-diffs even though the filter admits nothing --
    // the reset-then-baseline protocol (a reset is only trusted after a full diff).
    fill(p + kChunk, 8, 0x55);
    t.force_rebaseline();
    DirtySnapshot s2 = t.snapshot_pending(admit_nothing, nullptr);
    VKR_CHECK_EQ(s2.byte_count(), kChunk);
    VKR_CHECK_EQ(s2.uploads[0].ranges[0].offset, kChunk);
}

void test_filter_new_allocation_and_clamping() {
    std::string err;
    MappedMemoryTracker t;
    t.on_allocate(1, 2 * kChunk, 0, true, true);
    std::byte* p = t.on_map(1, 0, kWholeSize, err);
    fill(p, 8, 0x66);
    PageFilter admit_nothing = [](const std::byte*, std::uint64_t, std::vector<Range>&) {
        return true;
    };
    t.commit(t.snapshot_pending(admit_nothing, nullptr)); // alloc 1 baselined

    // A NEW allocation mapped later is full-diffed on ITS first sweep even though older
    // allocations are already narrowing (per-allocation baselines).
    t.on_allocate(2, kChunk, 0, true, true);
    std::byte* q = t.on_map(2, 0, kWholeSize, err);
    fill(q, 8, 0x77);
    SweepIo io;
    DirtySnapshot s = t.snapshot_pending(admit_nothing, &io);
    VKR_CHECK_EQ(s.byte_count(), kChunk); // alloc 2's dirty chunk ships (baseline)
    VKR_CHECK_EQ(s.uploads[0].memory, static_cast<std::uint64_t>(2));
    VKR_CHECK_EQ(io.diffed_bytes, kChunk); // alloc 1 contributed 0 (filtered), alloc 2 full
    t.commit(s);

    // Out-of-bounds / overflowing / empty filter ranges are clamped away without incident.
    fill(q, 8, 0x88);
    PageFilter weird = [](const std::byte*, std::uint64_t size, std::vector<Range>& out) {
        out.push_back(Range{size + 100, 50});  // beyond the end -> dropped
        out.push_back(Range{0, 0});            // empty -> dropped
        out.push_back(Range{size - 1, ~0ull}); // overflow -> clamped to the end
        return true;
    };
    DirtySnapshot s2 = t.snapshot_pending(weird, nullptr);
    // The clamped tail range covers the last chunk of alloc 2 -> its dirty content ships.
    VKR_CHECK_EQ(s2.byte_count(), kChunk);
}

void test_filter_failed_upload_forces_rebaseline() {
    // baseline_done flips at SNAPSHOT time, the mirror only at commit. A
    // baseline snapshot that FAILS to upload (no commit) leaves shadow != mirror while the
    // epoch's soft-dirty reset already consumed the kernel bits -- so a filtered sweep would
    // silently skip the still-differing bytes. The producer contract: on any failed/thrown
    // filtered-snapshot upload, force_rebaseline() before the next sweep.
    std::string err;
    MappedMemoryTracker t;
    t.on_allocate(1, 2 * kChunk, 0, true, true);
    std::byte* p = t.on_map(1, 0, kWholeSize, err);
    fill(p, 16, 0x5B);
    PageFilter admit_nothing = [](const std::byte*, std::uint64_t, std::vector<Range>&) {
        return true; // "nothing written since the reset" (true post-reset kernel answer)
    };

    // The baseline sweep finds the dirty chunk... and the upload FAILS (no commit).
    DirtySnapshot baseline = t.snapshot_pending(admit_nothing, nullptr);
    VKR_CHECK_EQ(baseline.byte_count(), kChunk);

    // THE TRAP, pinned as documentation: without the producer's force_rebaseline, the next
    // filtered sweep trusts the filter and the unacknowledged bytes go silently stale.
    DirtySnapshot trapped = t.snapshot_pending(admit_nothing, nullptr);
    VKR_CHECK(trapped.empty());

    // THE CURE: force_rebaseline -> the next sweep full-diffs and the bytes re-send.
    t.force_rebaseline();
    DirtySnapshot retry = t.snapshot_pending(admit_nothing, nullptr);
    VKR_CHECK_EQ(retry.byte_count(), kChunk);
    VKR_CHECK(retry.payload == baseline.payload);
    t.commit(retry);
    VKR_CHECK(t.snapshot_pending(admit_nothing, nullptr).empty());
}

#ifdef __linux__
// The REAL soft-dirty reader against the running kernel. SKIPs cleanly where the kernel lacks
// soft-dirty (init()'s functional self-test decides -- the same gate the ICD uses in
// production, so a skip here means the ICD would also have fallen back to full diffs).
void test_softdirty_real() {
    vkr::icd_softdirty::SoftDirtyTracker sd;
    if (!sd.init()) {
        std::fprintf(stderr,
                     "unit_icd_memory: soft-dirty unavailable on this kernel -- SKIPPING the "
                     "real-pagemap test (the ICD falls back to full-diff sweeps here)\n");
        return;
    }
    const std::uint64_t ps = static_cast<std::uint64_t>(::sysconf(_SC_PAGESIZE));
    const std::uint64_t n = 8;
    void* mem = ::mmap(nullptr, n * ps, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    VKR_CHECK(mem != MAP_FAILED);
#ifdef MADV_NOHUGEPAGE
    (void) ::madvise(mem, n * ps, MADV_NOHUGEPAGE); // keep 4K granularity deterministic
#endif
    auto* p = static_cast<volatile unsigned char*>(mem);
    for (std::uint64_t i = 0; i < n; ++i) {
        p[i * ps] = 1; // fault every page in
    }
    VKR_CHECK(sd.reset());
    p[2 * ps] = 2; // dirty pages 2 and 5 only
    p[5 * ps] = 2;

    std::vector<Range> out;
    VKR_CHECK(sd.read_dirty(mem, n * ps, out));
    auto covered = [&](std::uint64_t off) {
        for (const Range& r : out) {
            if (off >= r.offset && off < r.offset + r.size) {
                return true;
            }
        }
        return false;
    };
    // No false negatives (the correctness half of the contract):
    VKR_CHECK(covered(2 * ps));
    VKR_CHECK(covered(5 * ps));
    // And on plain 4K pages, untouched neighbors stay clean (resets actually work):
    for (const std::uint64_t i : {0ull, 1ull, 3ull, 4ull, 6ull, 7ull}) {
        VKR_CHECK(!covered(i * ps));
    }
    // Bits are STICKY until the next reset (the monotone-set protocol relies on it).
    std::vector<Range> again;
    VKR_CHECK(sd.read_dirty(mem, n * ps, again));
    VKR_CHECK(!again.empty());
    // A reset with no writes afterward reads fully clean.
    VKR_CHECK(sd.reset());
    std::vector<Range> clean;
    VKR_CHECK(sd.read_dirty(mem, n * ps, clean));
    VKR_CHECK(clean.empty());
    ::munmap(mem, n * ps);
}
#endif

} // namespace

int main() {
    test_coherence_gate();
    test_map_bounds_and_overflow();
    test_two_phase_commit_after_ack();
    test_commit_only_after_successful_upload();
    test_unmap_keeps_pending_then_clean();
    test_successful_upload_then_unmap_sends_nothing();
    test_prune_clean_unmapped_candidate();
    test_partial_mapping();
    test_whole_size_flush();
    test_explicit_flush_overflow_rejected();
    test_adjacent_chunk_coalescing();
    test_disjoint_chunks();
    test_multiple_allocations_sweep();
    test_incremental_diff_after_commit();
    test_free_discards();
    test_flush_outside_mapping();
    test_split_for_upload();
    test_split_partial_commit();
    test_filter_baseline_then_narrow();
    test_filter_error_and_rebaseline();
    test_filter_new_allocation_and_clamping();
    test_filter_failed_upload_forces_rebaseline();
#ifdef __linux__
    test_softdirty_real();
#endif
    return vkr::test::finish("unit_icd_memory");
}
