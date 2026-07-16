// The kernel soft-dirty page reader behind the upload sweep's PageFilter.
//
// The kernel sets a per-PTE "soft-dirty" bit on every write since the last reset
// (Documentation/admin-guide/mm/soft-dirty.rst): /proc/self/clear_refs <- "4" resets all bits;
// /proc/self/pagemap bit 55 reads them, unprivileged for the OWN process. This turns the
// sweep's "which shadow bytes might differ from the mirror" question into a page-table read
// instead of a full memcmp of every mapped allocation.
//
// Fail-closed by construction:
//   - init() runs a FUNCTIONAL self-test (a probe page written after a reset must read dirty;
//     one not written since must read clean) -- kernel config is never trusted on its own.
//   - Any later read/reset failure reports false, and the caller falls back to the full diff.
//   - False positives (page flagged but content equal) only cost a memcmp -- the content diff
//     stays the source of truth. False negatives cannot happen while the caller upholds the
//     protocol: a reset is ONLY ever issued immediately before a full-diff baseline sweep
//     (MappedMemoryTracker::force_rebaseline), so "bit clear" always means "not written since
//     a moment the content was fully diffed". THP only coarsens granularity (more positives).
//
// Linux-only (pagemap/clear_refs); the pure PageFilter seam it plugs into lives in
// icd_memory.h and is unit-tested with fakes on both platforms. The real reader is exercised
// by a Linux-only unit test that SKIPs cleanly when the kernel lacks soft-dirty.
#ifndef VKRELAY2_LINUX_ICD_ICD_SOFTDIRTY_H
#define VKRELAY2_LINUX_ICD_ICD_SOFTDIRTY_H

#include "linux/icd/icd_memory.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>
#include <vector>

namespace vkr::icd_softdirty {

class SoftDirtyTracker {
  public:
    ~SoftDirtyTracker() {
        if (pagemap_fd_ >= 0) {
            ::close(pagemap_fd_);
        }
        if (clear_fd_ >= 0) {
            ::close(clear_fd_);
        }
    }
    SoftDirtyTracker() = default;
    SoftDirtyTracker(const SoftDirtyTracker&) = delete;
    SoftDirtyTracker& operator=(const SoftDirtyTracker&) = delete;

    bool available() const { return available_; }

    // Opens the proc files and proves soft-dirty WORKS here (write-after-reset reads dirty,
    // untouched-since-reset reads clean). Any doubt -> false, and the tracker stays unusable.
    bool init() {
        available_ = false;
        page_size_ = static_cast<std::uint64_t>(::sysconf(_SC_PAGESIZE));
        if (page_size_ == 0 || (page_size_ & (page_size_ - 1)) != 0) {
            return false;
        }
        pagemap_fd_ = ::open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
        clear_fd_ = ::open("/proc/self/clear_refs", O_WRONLY | O_CLOEXEC);
        if (pagemap_fd_ < 0 || clear_fd_ < 0) {
            return false;
        }
        // Functional probe: two private pages, both faulted in by writes; reset; rewrite only
        // the first. The first MUST read dirty (else writes are not tracked); the second MUST
        // read clean (else resets do not work and the filter could never narrow anything).
        void* probe = ::mmap(nullptr, 2 * page_size_, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (probe == MAP_FAILED) {
            return false;
        }
        volatile char* p = static_cast<volatile char*>(probe);
        p[0] = 1;
        p[page_size_] = 1;
        bool ok = reset();
        if (ok) {
            p[0] = 2;
            std::uint64_t ent0 = 0;
            std::uint64_t ent1 = 0;
            ok = read_entry(probe, &ent0) &&
                 read_entry(static_cast<const char*>(probe) + page_size_, &ent1) &&
                 (ent0 & kSoftDirtyBit) != 0 && (ent1 & kSoftDirtyBit) == 0;
        }
        ::munmap(probe, 2 * page_size_);
        available_ = ok;
        return ok;
    }

    // Resets ALL soft-dirty bits of this process. The caller MUST follow with a full-diff
    // baseline sweep (force_rebaseline) before trusting any subsequent read. false = failed
    // (caller disables the filter; nothing was half-trusted).
    bool reset() {
        if (clear_fd_ < 0) {
            return false;
        }
        return ::pwrite(clear_fd_, "4", 1, 0) == 1;
    }

    // Appends the possibly-written page ranges of [base, base+size), RELATIVE to base, to
    // `out` (coalesced). false = could not read (caller full-diffs this span).
    bool read_dirty(const void* base, std::uint64_t size, std::vector<vkr::icd_mem::Range>& out) {
        if (!available_ || size == 0) {
            return false;
        }
        const std::uint64_t addr = reinterpret_cast<std::uintptr_t>(base);
        if (size > ~0ull - addr) {
            return false; // addr+size would overflow: no answer -> the caller full-diffs
        }
        const std::uint64_t first_page = addr / page_size_;
        const std::uint64_t last_page = (addr + size - 1) / page_size_;
        std::uint64_t page = first_page;
        bool run_open = false;
        std::uint64_t run_start = 0; // byte offset relative to base
        buf_.resize(kBatchEntries);
        while (page <= last_page) {
            const std::uint64_t batch =
                std::min<std::uint64_t>(kBatchEntries, last_page - page + 1);
            const ::ssize_t want = static_cast<::ssize_t>(batch * sizeof(std::uint64_t));
            const ::ssize_t got = ::pread(pagemap_fd_, buf_.data(), static_cast<std::size_t>(want),
                                          static_cast<::off_t>(page * sizeof(std::uint64_t)));
            if (got != want) {
                return false; // short read = no answer; the caller diffs the whole span
            }
            for (std::uint64_t i = 0; i < batch; ++i) {
                const bool dirty = (buf_[i] & kSoftDirtyBit) != 0;
                const std::uint64_t pg = page + i;
                // Page bounds clamped to [base, base+size), relative to base.
                const std::uint64_t lo = pg * page_size_ > addr ? pg * page_size_ - addr : 0;
                const std::uint64_t hi = std::min((pg + 1) * page_size_ - addr, size);
                if (dirty) {
                    if (!run_open) {
                        run_open = true;
                        run_start = lo;
                    }
                } else if (run_open) {
                    out.push_back(vkr::icd_mem::Range{run_start, lo - run_start});
                    run_open = false;
                }
                if (dirty && pg == last_page) {
                    out.push_back(vkr::icd_mem::Range{run_start, hi - run_start});
                    run_open = false;
                }
            }
            page += batch;
        }
        return true;
    }

  private:
    static constexpr std::uint64_t kSoftDirtyBit = 1ull << 55;
    static constexpr std::uint64_t kBatchEntries = 8192; // 64 KiB pagemap read per batch

    // One page's raw pagemap entry (self-test helper).
    bool read_entry(const void* addr, std::uint64_t* ent) {
        const std::uint64_t page = reinterpret_cast<std::uintptr_t>(addr) / page_size_;
        return ::pread(pagemap_fd_, ent, sizeof(*ent),
                       static_cast<::off_t>(page * sizeof(std::uint64_t))) == sizeof(*ent);
    }

    int pagemap_fd_ = -1;
    int clear_fd_ = -1;
    std::uint64_t page_size_ = 0;
    bool available_ = false;
    std::vector<std::uint64_t> buf_;
};

} // namespace vkr::icd_softdirty

#endif // VKRELAY2_LINUX_ICD_ICD_SOFTDIRTY_H
