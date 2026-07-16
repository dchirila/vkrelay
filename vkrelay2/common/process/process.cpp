// Platform-independent pieces of the process module: the stdout LineQueue.
// Process itself is defined per platform in process_win.cpp / process_posix.cpp.
#include "common/process/line_queue.hpp"

#include <chrono>

namespace vkr::process {

void LineQueue::push_bytes(const char* data, std::size_t n) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (std::size_t i = 0; i < n; ++i) {
        const char c = data[i];
        if (c == '\n') {
            if (!partial_.empty() && partial_.back() == '\r') {
                partial_.pop_back();
            }
            lines_.push_back(std::move(partial_));
            partial_.clear();
        } else {
            partial_.push_back(c);
        }
    }
    cv_.notify_all();
}

void LineQueue::mark_eof() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!partial_.empty()) {
        if (partial_.back() == '\r') {
            partial_.pop_back();
        }
        lines_.push_back(std::move(partial_));
        partial_.clear();
    }
    eof_ = true;
    cv_.notify_all();
}

bool LineQueue::pop_line(int timeout_ms, std::string& line) {
    std::unique_lock<std::mutex> lock(mutex_);
    auto has_data = [this] { return !lines_.empty() || eof_; };
    if (timeout_ms < 0) {
        cv_.wait(lock, has_data);
    } else {
        cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), has_data);
    }
    if (lines_.empty()) {
        return false; // EOF with nothing left, or timed out
    }
    line = std::move(lines_.front());
    lines_.pop_front();
    return true;
}

std::string LineQueue::drain_to_end() {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return eof_; });
    std::string out;
    for (std::size_t i = 0; i < lines_.size(); ++i) {
        if (i != 0) {
            out.push_back('\n');
        }
        out += lines_[i];
    }
    lines_.clear();
    return out;
}

} // namespace vkr::process
