// Versioned launch descriptor.
//
// argv and env stay arrays/maps end to end; they are NEVER collapsed into a
// shell command string. The descriptor crosses process / OS boundaries as
// structured data (JSON here), and is validated before any worker or target
// process is launched.
#ifndef VKRELAY2_COMMON_PROTOCOL_LAUNCH_DESCRIPTOR_HPP
#define VKRELAY2_COMMON_PROTOCOL_LAUNCH_DESCRIPTOR_HPP

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace vkr::protocol {

constexpr int kLaunchDescriptorVersion = 1;
constexpr std::size_t kLaunchDescriptorMaxBytes = 1u << 20; // 1 MiB safety bound

struct DescriptorError : std::runtime_error {
    explicit DescriptorError(const std::string& msg) : std::runtime_error(msg) {}
};

struct SessionOptions {
    std::string gpu_selector = "auto";      // GPU selector text
    std::string display_backend = "auto";   // auto | x11 | wayland
    std::string graphics_frontend = "auto"; // auto | vulkan13 | opengl46zink
};

struct LaunchDescriptor {
    int version = kLaunchDescriptorVersion;
    SessionOptions session;
    std::string cwd; // empty => inherit launcher cwd
    std::vector<std::string> argv;
    std::vector<std::pair<std::string, std::string>> env; // ordered env overrides

    // Throws DescriptorError if the descriptor is not safe to launch.
    void validate() const;

    std::string to_json(bool pretty = false) const;
    static LaunchDescriptor from_json(const std::string& text);
};

// True if `s` is well-formed UTF-8 with no embedded NUL.
bool is_clean_utf8(const std::string& s);

} // namespace vkr::protocol

#endif // VKRELAY2_COMMON_PROTOCOL_LAUNCH_DESCRIPTOR_HPP
