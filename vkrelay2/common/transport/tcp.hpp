// TCP implementation of the transport Connection / Listener interfaces.
// Loopback-oriented for the bootstrap (the supervisor binds 127.0.0.1).
#ifndef VKRELAY2_COMMON_TRANSPORT_TCP_HPP
#define VKRELAY2_COMMON_TRANSPORT_TCP_HPP

#include "common/transport/transport.hpp"

#include <memory>
#include <string>

namespace vkr::transport {

// Connects to host:port (TCP_NODELAY, blocking after connect). Uses a
// non-blocking connect with a deadline so an unreachable host fails fast.
std::unique_ptr<Connection> tcp_connect(const std::string& host, int port, int timeout_ms = 5000);

// Binds a 127.0.0.1 listener on `port` (0 => ephemeral) and starts listening.
// Use Listener::port() to learn the bound port.
std::unique_ptr<Listener> tcp_listen(int port);

// As above, but binds `bind_address` (a numeric IPv4, e.g. "127.0.0.1" for
// loopback or "0.0.0.0" for all interfaces -- the latter lets NAT-mode WSL reach a
// Windows daemon). An empty string means loopback. Throws on an invalid address.
std::unique_ptr<Listener> tcp_listen(int port, const std::string& bind_address);

} // namespace vkr::transport

#endif // VKRELAY2_COMMON_TRANSPORT_TCP_HPP
