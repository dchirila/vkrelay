// Stable id allocation.
//
// Identity is split deliberately:
//   * supervisor_session_id -- one per supervisor instance (process lifetime).
//   * worker_id             -- one per accepted connection / worker.
//   * app_instance_id       -- supplied by the client in its Hello (per app).
// Routing keys off worker_id / app_instance_id, never the supervisor session
// id. Ids are process-unique and human-greppable in logs.
#ifndef VKRELAY2_COMMON_PROTOCOL_IDS_HPP
#define VKRELAY2_COMMON_PROTOCOL_IDS_HPP

#include <atomic>
#include <cstdint>
#include <string>

namespace vkr::protocol {

class IdAllocator {
  public:
    IdAllocator(); // mints a process-unique supervisor session id
    explicit IdAllocator(std::string supervisor_session_id);

    const std::string& supervisor_session_id() const { return supervisor_session_id_; }
    std::string next_worker_id(); // thread-safe

  private:
    std::string supervisor_session_id_;
    std::atomic<std::uint64_t> next_worker_{1};
};

} // namespace vkr::protocol

#endif // VKRELAY2_COMMON_PROTOCOL_IDS_HPP
