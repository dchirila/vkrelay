// the well-known daemon endpoint resolves from env with sensible
// defaults, so neither the WSL client nor the Windows daemon needs a typed
// host:port. Verifies the defaults and the env overrides (including bad input).
#include "common/control/daemon_endpoint.hpp"

#include "tests/test_assert.hpp"

#include <cstdlib>
#include <string>

using namespace vkr;

namespace {

#if defined(_WIN32)
void set_env(const char* key, const char* value) {
    _putenv_s(key, value);
}
void unset_env(const char* key) {
    _putenv_s(key, ""); // empty removes the variable on Windows
}
#else
void set_env(const char* key, const char* value) {
    setenv(key, value, 1);
}
void unset_env(const char* key) {
    unsetenv(key);
}
#endif

void clear_all() {
    unset_env("VKRELAY2_DAEMON_HOST");
    unset_env("VKRELAY2_DAEMON_PORT");
    unset_env("VKRELAY2_BIND");
    unset_env("VKRELAY2_CONTROL_TIMEOUT_MS");
    unset_env("VKRELAY2_SERVE_IDLE_TIMEOUT_MS");
}

} // namespace

int main() {
    clear_all();

    // Defaults: well-known port, loopback host + bind.
    VKR_CHECK_EQ(control::default_daemon_port(), 13579);
    VKR_CHECK_EQ(control::default_daemon_host(), std::string("127.0.0.1"));
    VKR_CHECK_EQ(control::default_bind_address(), std::string("127.0.0.1"));

    // Port override + rejection of bad values (fall back to the default).
    set_env("VKRELAY2_DAEMON_PORT", "24680");
    VKR_CHECK_EQ(control::default_daemon_port(), 24680);
    set_env("VKRELAY2_DAEMON_PORT", "not-a-number");
    VKR_CHECK_EQ(control::default_daemon_port(), 13579);
    set_env("VKRELAY2_DAEMON_PORT", "70000"); // out of range
    VKR_CHECK_EQ(control::default_daemon_port(), 13579);
    set_env("VKRELAY2_DAEMON_PORT", "0"); // must be > 0
    VKR_CHECK_EQ(control::default_daemon_port(), 13579);
    set_env("VKRELAY2_DAEMON_PORT", "12345trailing"); // trailing junk
    VKR_CHECK_EQ(control::default_daemon_port(), 13579);

    // Host + bind overrides.
    set_env("VKRELAY2_DAEMON_HOST", "10.1.2.3");
    VKR_CHECK_EQ(control::default_daemon_host(), std::string("10.1.2.3"));
    set_env("VKRELAY2_BIND", "0.0.0.0");
    VKR_CHECK_EQ(control::default_bind_address(), std::string("0.0.0.0"));

    // control-plane timeout knobs: defaults, valid override, and rejection of bad values.
    VKR_CHECK_EQ(control::default_control_timeout_ms(), 15000);
    VKR_CHECK_EQ(control::default_serve_idle_timeout_ms(), 30000);
    set_env("VKRELAY2_CONTROL_TIMEOUT_MS", "5000");
    VKR_CHECK_EQ(control::default_control_timeout_ms(), 5000);
    set_env("VKRELAY2_SERVE_IDLE_TIMEOUT_MS", "60000");
    VKR_CHECK_EQ(control::default_serve_idle_timeout_ms(), 60000);
    for (const char* bad : {"not-a-number", "0", "-1", "12345trailing", ""}) {
        set_env("VKRELAY2_CONTROL_TIMEOUT_MS", bad);
        set_env("VKRELAY2_SERVE_IDLE_TIMEOUT_MS", bad);
        VKR_CHECK_EQ(control::default_control_timeout_ms(), 15000);
        VKR_CHECK_EQ(control::default_serve_idle_timeout_ms(), 30000);
    }

    clear_all();
    return vkr::test::finish("unit_daemon_endpoint");
}
