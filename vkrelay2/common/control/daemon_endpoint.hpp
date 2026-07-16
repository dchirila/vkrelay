// Well-known vkrelay2 control-plane endpoint.
//
// A single long-lived daemon listens on one well-known port, so the WSL client
// and the Windows daemon agree without anyone typing host:port. Defaults match the
// original vkrelay (port 13579, host 127.0.0.1) and are overridable by env for the
// NAT-mode WSL case where loopback does not cross the boundary:
//   VKRELAY2_DAEMON_HOST  client: where to reach the daemon (default 127.0.0.1)
//   VKRELAY2_DAEMON_PORT  both:   the control-plane port    (default 13579)
//   VKRELAY2_BIND         daemon: bind address              (default 127.0.0.1;
//                                 set 0.0.0.0 for NAT-mode WSL)
#ifndef VKRELAY2_COMMON_CONTROL_DAEMON_ENDPOINT_HPP
#define VKRELAY2_COMMON_CONTROL_DAEMON_ENDPOINT_HPP

#include <string>

namespace vkr::control {

// The well-known default control-plane port (shared by the original vkrelay).
constexpr int kDefaultDaemonPort = 13579;

// Client-side: the host to reach the daemon at (VKRELAY2_DAEMON_HOST or loopback).
std::string default_daemon_host();

// The control-plane port (VKRELAY2_DAEMON_PORT or kDefaultDaemonPort). A missing /
// malformed / out-of-range value falls back to the default.
int default_daemon_port();

// Daemon-side: the bind address (VKRELAY2_BIND or loopback). "0.0.0.0" exposes the
// daemon to NAT-mode WSL; loopback keeps it host-local.
std::string default_bind_address();

// Launcher/session reliability: bounded control-plane reads so a wedged or
// slow daemon fails CLOSED instead of hanging the bring-up. A missing/malformed/non-positive value
// falls back to the default in every case.
//
// Client-side per-read deadline for a control session (VKRELAY2_CONTROL_TIMEOUT_MS, default 15000).
// Covers waiting for HelloAck and SessionStarted; generous enough for a real worker spawn yet
// bounded so a stuck handshake throws rather than blocking forever.
int default_control_timeout_ms();

// Supervisor-side per-connection idle deadline (VKRELAY2_SERVE_IDLE_TIMEOUT_MS, default 30000). The
// control listener serves one short-lived launcher session at a time; a stalled/half-open client
// must not monopolize the loop, so an accepted connection whose next read blocks longer than this
// is dropped (the loop returns to accept). Longer than a legitimate launch round-trip.
int default_serve_idle_timeout_ms();

} // namespace vkr::control

#endif // VKRELAY2_COMMON_CONTROL_DAEMON_ENDPOINT_HPP
