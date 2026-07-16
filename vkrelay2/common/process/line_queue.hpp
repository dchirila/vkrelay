// Thread-safe line buffer fed by a stdout reader thread and drained by the
// owning Process. Internal to the process module.
#ifndef VKRELAY2_COMMON_PROCESS_LINE_QUEUE_HPP
#define VKRELAY2_COMMON_PROCESS_LINE_QUEUE_HPP

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <string>

namespace vkr::process {

class LineQueue {
  public:
    // Called from the reader thread with freshly read bytes.
    void push_bytes(const char* data, std::size_t n);
    // Called from the reader thread when the pipe reaches EOF.
    void mark_eof();

    // Pops the next complete line (without trailing newline). Returns false if
    // EOF was reached with no more lines, or the timeout elapsed first.
    bool pop_line(int timeout_ms, std::string& line);

    // Blocks until EOF, then returns all remaining lines joined by '\n'.
    std::string drain_to_end();

  private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::string> lines_;
    std::string partial_;
    bool eof_ = false;
};

} // namespace vkr::process

#endif // VKRELAY2_COMMON_PROCESS_LINE_QUEUE_HPP
