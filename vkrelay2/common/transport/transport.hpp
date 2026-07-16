// Transport abstraction for the vkrelay2 control plane. TCP first (tcp.hpp);
// vsock / named pipe are future implementations of the same Connection /
// Listener interfaces.
#ifndef VKRELAY2_COMMON_TRANSPORT_TRANSPORT_HPP
#define VKRELAY2_COMMON_TRANSPORT_TRANSPORT_HPP

#include "common/protocol/ids.hpp"
#include "common/protocol/messages.hpp"
#include "common/util/json.hpp"

#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>

namespace vkr::transport {

struct TransportError : std::runtime_error {
    explicit TransportError(const std::string& msg) : std::runtime_error(msg) {}
};

// Reliable, ordered byte stream. read_some returns >0 on data and 0 on EOF;
// write_all writes every byte. Both throw TransportError on error.
class Connection {
  public:
    virtual ~Connection() = default;
    virtual std::size_t read_some(void* buf, std::size_t len) = 0;
    virtual void write_all(const void* buf, std::size_t len) = 0;
    virtual void close() = 0;
    // Unblocks a read/write blocked on another thread (so a handler stuck in
    // recv can be released during shutdown) without closing the socket; the
    // owner still calls close().
    virtual void cancel() = 0;
    // Sets a receive deadline: a read_some that would block longer than
    // timeout_ms instead throws TransportError (a timed-out read). timeout_ms
    // <= 0 disables the deadline (block indefinitely). Default: no-op for
    // transports that cannot enforce one.
    virtual void set_read_timeout(int timeout_ms) { (void) timeout_ms; }
    // Waits up to timeout_ms for the PEER to close its end of the connection (a
    // received FIN/RST), WITHOUT consuming any payload bytes and without
    // changing the blocking mode read_some uses. Returns true if peer-close is
    // observed, false on timeout. It is the primitive an out-of-band liveness
    // observer uses to notice the app died while the RPC reader is blocked
    // elsewhere (the WSI-pump wedge), so it is safe to call CONCURRENTLY with the
    // single thread doing read_some/write_all on the same connection. CONTRACT:
    // the caller MUST NOT race a LOCAL close()/cancel()/destruction of this
    // Connection against an in-flight wait_peer_closed (it does not synchronize
    // with those) -- stop/join the observer before tearing the connection down.
    // Transports that cannot observe peer-close return false (an observer then
    // falls back to the normal EOF path).
    virtual bool wait_peer_closed(int timeout_ms) {
        (void) timeout_ms;
        return false;
    }
};

class Listener {
  public:
    virtual ~Listener() = default;
    virtual std::unique_ptr<Connection> accept() = 0;
    virtual int port() const = 0; // bound port (resolves an ephemeral :0)
    virtual void close() = 0;
};

// Frames + JSON messages over a Connection. Owns the leftover read buffer so a
// read that straddles a frame boundary is handled correctly across calls.
class MessageChannel {
  public:
    explicit MessageChannel(Connection& conn) : conn_(conn) {}

    void send(protocol::MessageType type, const json::Value& body);
    // Reads one message. Returns false on a clean EOF with no partial frame
    // buffered; throws TransportError on a truncated frame or decode failure.
    bool recv(protocol::MessageType& type, json::Value& body);

  private:
    Connection& conn_;
    std::string buffer_;
};

struct AcceptedSession {
    protocol::ClientHello client;
    protocol::ServerHello server;
};

// Server side: expects a Hello, validates the protocol version, mints worker
// id, replies HelloAck. `worker_backend` ("mock" | "real") is advertised in the
// HelloAck so a client can verify the daemon's session-worker backend before
// launching. `host_display` advertises the host's addressable window space (primary-monitor
// work area, physical px) so the launcher can size the private guest X root to it; all-zero =
// unknown (the default). On any failure it sends an Error message and throws.
AcceptedSession server_handshake(MessageChannel& channel, protocol::IdAllocator& ids,
                                 const std::string& worker_backend = "mock",
                                 const protocol::HostDisplay& host_display = {},
                                 const display::DisplayLayoutDecodeResult& display_layout = {});

// Client side: sends Hello and returns the HelloAck. Throws TransportError if
// the server replies Error or the protocol version does not match.
protocol::ServerHello client_handshake(MessageChannel& channel, const protocol::ClientHello& hello);

} // namespace vkr::transport

#endif // VKRELAY2_COMMON_TRANSPORT_TRANSPORT_HPP
