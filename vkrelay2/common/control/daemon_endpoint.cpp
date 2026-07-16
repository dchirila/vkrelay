#include "common/control/daemon_endpoint.hpp"

#include <cstdlib>
#include <string>

namespace vkr::control {
namespace {

// Reads an env var, returning `fallback` when it is unset or empty.
std::string env_or(const char* name, const std::string& fallback) {
    const char* value = std::getenv(name);
    return (value != nullptr && value[0] != '\0') ? std::string(value) : fallback;
}

// Reads a positive-integer (milliseconds) env var; a missing/malformed/non-positive value (or one
// past INT range) falls back. Shared by the two control-plane timeout knobs.
int env_positive_int_or(const char* name, int fallback) {
    const std::string text = env_or(name, std::string());
    if (!text.empty()) {
        char* end = nullptr;
        const long value = std::strtol(text.c_str(), &end, 10);
        if (end != text.c_str() && *end == '\0' && value > 0 && value <= 2147483647L) {
            return static_cast<int>(value);
        }
    }
    return fallback;
}

} // namespace

std::string default_daemon_host() {
    return env_or("VKRELAY2_DAEMON_HOST", "127.0.0.1");
}

int default_daemon_port() {
    const std::string text = env_or("VKRELAY2_DAEMON_PORT", std::string());
    if (!text.empty()) {
        char* end = nullptr;
        const long port = std::strtol(text.c_str(), &end, 10);
        if (end != text.c_str() && *end == '\0' && port > 0 && port < 65536) {
            return static_cast<int>(port);
        }
    }
    return kDefaultDaemonPort;
}

std::string default_bind_address() {
    return env_or("VKRELAY2_BIND", "127.0.0.1");
}

int default_control_timeout_ms() {
    return env_positive_int_or("VKRELAY2_CONTROL_TIMEOUT_MS", 15000);
}

int default_serve_idle_timeout_ms() {
    return env_positive_int_or("VKRELAY2_SERVE_IDLE_TIMEOUT_MS", 30000);
}

} // namespace vkr::control
