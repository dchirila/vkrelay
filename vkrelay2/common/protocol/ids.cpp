#include "common/protocol/ids.hpp"

#include <array>
#include <cstdio>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace vkr::protocol {
namespace {

unsigned long current_pid() {
#if defined(_WIN32)
    return static_cast<unsigned long>(::GetCurrentProcessId());
#else
    return static_cast<unsigned long>(::getpid());
#endif
}

// Monotonic counter so two IdAllocators in one process still differ.
std::atomic<std::uint64_t> g_session_counter{1};

} // namespace

IdAllocator::IdAllocator() {
    const std::uint64_t n = g_session_counter.fetch_add(1, std::memory_order_relaxed);
    std::array<char, 48> buf{};
    std::snprintf(buf.data(), buf.size(), "sup-%lx-%llu", current_pid(),
                  static_cast<unsigned long long>(n));
    supervisor_session_id_ = buf.data();
}

IdAllocator::IdAllocator(std::string supervisor_session_id)
    : supervisor_session_id_(std::move(supervisor_session_id)) {}

std::string IdAllocator::next_worker_id() {
    const std::uint64_t n = next_worker_.fetch_add(1, std::memory_order_relaxed);
    return "wkr-" + std::to_string(n);
}

} // namespace vkr::protocol
