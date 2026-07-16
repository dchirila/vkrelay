#if !defined(_WIN32)
#ifndef _GNU_SOURCE
#define _GNU_SOURCE // SOCK_CLOEXEC, accept4
#endif
#endif

#include "common/transport/tcp.hpp"

#include <cstring>
#include <mutex>
#include <string>

#if defined(_WIN32)
#include <winsock2.h>

#include <windows.h> // CancelIoEx (after winsock2.h, per the documented order)
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")
using socket_t = SOCKET;
namespace {
constexpr socket_t kInvalid = INVALID_SOCKET;
}
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h> // POLLRDHUP (peer-close detection); needs _GNU_SOURCE (set above)
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
namespace {
constexpr socket_t kInvalid = -1;
}
#endif

namespace vkr::transport {
namespace {

#if defined(_WIN32)
void ensure_winsock() {
    static const bool ok = [] {
        WSADATA data;
        return ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    if (!ok) {
        throw TransportError("WSAStartup failed");
    }
}
void close_socket(socket_t s) {
    ::closesocket(s);
}
// Creating sockets non-inheritable: spawned workers (posix_spawn / CreateProcess)
// must not inherit the supervisor's listening / accepted sockets, or a closed
// peer never reaches EOF. On Windows our spawn uses bInheritHandles=FALSE, so
// default sockets are fine; on POSIX we need SOCK_CLOEXEC / accept4 (atomic,
// no fork race).
socket_t create_socket(int family, int type, int proto) {
    return ::socket(family, type, proto);
}
socket_t accept_socket(socket_t listen_sock) {
    return ::accept(listen_sock, nullptr, nullptr);
}

// closesocket() already unblocks a thread blocked in accept() on Windows.
void wake_accept(socket_t) {}
// Unblocks a recv/send already blocked on another thread WITHOUT closing the
// socket. On Windows shutdown() does NOT unblock an in-flight recv; CancelIoEx
// cancels it (the recv returns WSA_OPERATION_ABORTED). shutdown() then blocks
// further I/O.
void cancel_pending_io(socket_t s) {
    ::CancelIoEx(reinterpret_cast<HANDLE>(s), nullptr);
    ::shutdown(s, SD_BOTH);
}
int sock_errno() {
    return ::WSAGetLastError();
}
bool is_timeout_err(int err) {
    return err == WSAETIMEDOUT;
}
void set_nonblocking(socket_t s, bool on) {
    u_long mode = on ? 1 : 0;
    ::ioctlsocket(s, FIONBIO, &mode);
}
// SO_RCVTIMEO takes a DWORD of milliseconds on Windows.
void set_recv_timeout(socket_t s, int timeout_ms) {
    DWORD ms = timeout_ms > 0 ? static_cast<DWORD>(timeout_ms) : 0;
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms), sizeof(ms));
}
// Non-consuming peer-close detection. Windows has no POLLRDHUP; a closed socket
// selects readable, so select() + a 1-byte MSG_PEEK distinguishes a graceful
// close (peek == 0) from pending data (peek > 0). NOTE: a graceful close QUEUED
// BEHIND unread payload is not observable this way without WSAEventSelect, which
// would force the socket non-blocking and break the concurrent blocking reader.
// The synchronous request/response RPC protocol never leaves unread payload
// before a FIN in the wedge case this serves (the guest is blocked awaiting a
// response), so the observer is correct-by-construction for that scenario.
bool poll_peer_closed(socket_t s, int timeout_ms) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(s, &rfds);
    timeval tv{};
    timeval* ptv = nullptr;
    if (timeout_ms >= 0) {
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        ptv = &tv;
    }
    const int r = ::select(0, &rfds, nullptr, nullptr, ptv); // nfds ignored on Windows
    if (r <= 0) {
        return false; // timeout, or select error -- caller retries or bails
    }
    char b = 0;
    const int n = ::recv(s, &b, 1, MSG_PEEK);
    if (n == 0) {
        return true; // graceful close (FIN, no pending payload)
    }
    if (n < 0) {
        const int err = ::WSAGetLastError();
        return err != WSAEWOULDBLOCK && err != WSAETIMEDOUT; // reset/aborted => gone
    }
    return false; // real payload pending -> not (yet) closed
}
#else
void ensure_winsock() {}
void close_socket(socket_t s) {
    ::close(s);
}
// On Linux, close() does NOT unblock a thread blocked in accept(); shutdown()
// does (accept then returns with an error). Call this before close_socket().
void wake_accept(socket_t s) {
    ::shutdown(s, SHUT_RDWR);
}
// POSIX sockets are inherited by posix_spawn'd children unless close-on-exec;
// SOCK_CLOEXEC / accept4 set it atomically so a launched worker never holds a
// copy of the supervisor's sockets.
socket_t create_socket(int family, int type, int proto) {
    return ::socket(family, type | SOCK_CLOEXEC, proto);
}
socket_t accept_socket(socket_t listen_sock) {
    return ::accept4(listen_sock, nullptr, nullptr, SOCK_CLOEXEC);
}

// On Linux, shutdown() unblocks an in-flight recv (returns 0); close() does not.
void cancel_pending_io(socket_t s) {
    ::shutdown(s, SHUT_RDWR);
}
int sock_errno() {
    return errno;
}
bool is_timeout_err(int err) {
    return err == EAGAIN || err == EWOULDBLOCK;
}
void set_nonblocking(socket_t s, bool on) {
    int flags = ::fcntl(s, F_GETFL, 0);
    if (flags < 0) {
        return;
    }
    ::fcntl(s, F_SETFL, on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
}
// SO_RCVTIMEO takes a struct timeval on POSIX.
void set_recv_timeout(socket_t s, int timeout_ms) {
    timeval tv{};
    if (timeout_ms > 0) {
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
    }
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}
// Non-consuming peer-close detection. POLLRDHUP reports the peer's FIN even when
// unread payload is still queued (the strong contract), so this needs no MSG_PEEK
// and never consumes bytes. POLLHUP/POLLERR/POLLNVAL also mean the peer is gone.
bool poll_peer_closed(socket_t s, int timeout_ms) {
    pollfd pfd{};
    pfd.fd = s;
    pfd.events = POLLRDHUP;
    pfd.revents = 0;
    const int r = ::poll(&pfd, 1, timeout_ms);
    if (r <= 0) {
        return false; // timeout, or EINTR/error -- caller retries or bails
    }
    return (pfd.revents & (POLLRDHUP | POLLHUP | POLLERR | POLLNVAL)) != 0;
}
#endif

void set_nodelay(socket_t s) {
    int one = 1;
    ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
}

int as_int_len(std::size_t len) {
    constexpr std::size_t kCap = 1u << 20;
    return static_cast<int>(len < kCap ? len : kCap);
}

class TcpConnection : public Connection {
  public:
    explicit TcpConnection(socket_t sock) : sock_(sock) {}
    ~TcpConnection() override { close(); }

    std::size_t read_some(void* buf, std::size_t len) override {
        const int n = ::recv(sock_, static_cast<char*>(buf), as_int_len(len), 0);
        if (n == 0) {
            return 0; // peer closed
        }
        if (n < 0) {
            const int err = sock_errno();
            if (is_timeout_err(err)) {
                throw TransportError("recv timed out");
            }
            throw TransportError("recv failed (err=" + std::to_string(err) + ")");
        }
        return static_cast<std::size_t>(n);
    }

    void write_all(const void* buf, std::size_t len) override {
        const char* p = static_cast<const char*>(buf);
        std::size_t left = len;
        while (left > 0) {
            const int n = ::send(sock_, p, as_int_len(left), 0);
            if (n <= 0) {
                throw TransportError("send failed (err=" + std::to_string(sock_errno()) + ")");
            }
            p += n;
            left -= static_cast<std::size_t>(n);
        }
    }

    // Unblocks a concurrent recv/send without closing the socket. Mutually
    // exclusive with close() so we never shutdown a descriptor close() just
    // recycled (which could hit an unrelated reused fd).
    void cancel() override {
        std::lock_guard<std::mutex> lock(sock_mutex_);
        if (sock_ != kInvalid) {
            cancel_pending_io(sock_);
        }
    }

    void set_read_timeout(int timeout_ms) override {
        std::lock_guard<std::mutex> lock(sock_mutex_);
        if (sock_ != kInvalid) {
            set_recv_timeout(sock_, timeout_ms);
        }
    }

    // Snapshot the fd under the lock (we do NOT hold it across the blocking
    // poll/select -- that would stall read_some for the whole timeout). Per the
    // interface contract, the caller must not race a LOCAL close() against an
    // in-flight wait_peer_closed, so the snapshot cannot be recycled underneath
    // us here; we make no claim about a stale/recycled fd (a recycled value could
    // name an unrelated live socket -- it is the caller's join-before-close that
    // rules that out, not this method).
    bool wait_peer_closed(int timeout_ms) override {
        socket_t s;
        {
            std::lock_guard<std::mutex> lock(sock_mutex_);
            s = sock_;
        }
        if (s == kInvalid) {
            return true; // our side already closed -> treat as closed (teardown)
        }
        return poll_peer_closed(s, timeout_ms);
    }

    void close() override {
        std::lock_guard<std::mutex> lock(sock_mutex_);
        if (sock_ != kInvalid) {
            close_socket(sock_);
            sock_ = kInvalid;
        }
    }

  private:
    std::mutex sock_mutex_;
    socket_t sock_;
};

class TcpListener : public Listener {
  public:
    TcpListener(socket_t sock, int port) : sock_(sock), port_(port) {}
    ~TcpListener() override { close(); }

    std::unique_ptr<Connection> accept() override {
        const socket_t client = accept_socket(sock_);
        if (client == kInvalid) {
            throw TransportError("accept failed (err=" + std::to_string(sock_errno()) + ")");
        }
        set_nodelay(client);
        return std::unique_ptr<Connection>(new TcpConnection(client));
    }

    int port() const override { return port_; }

    void close() override {
        if (sock_ != kInvalid) {
            wake_accept(sock_); // unblock a concurrent accept() (POSIX)
            close_socket(sock_);
            sock_ = kInvalid;
        }
    }

  private:
    socket_t sock_;
    int port_;
};

bool wait_writable(socket_t s, int timeout_ms) {
    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(s, &wfds);
    timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    const int rc = ::select(static_cast<int>(s) + 1, nullptr, &wfds, nullptr, &tv);
    return rc > 0 && FD_ISSET(s, &wfds);
}

} // namespace

std::unique_ptr<Connection> tcp_connect(const std::string& host, int port, int timeout_ms) {
    ensure_winsock();

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* result = nullptr;
    const std::string port_str = std::to_string(port);
    if (::getaddrinfo(host.c_str(), port_str.c_str(), &hints, &result) != 0 || result == nullptr) {
        throw TransportError("cannot resolve " + host + ":" + port_str);
    }

    socket_t sock = create_socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (sock == kInvalid) {
        ::freeaddrinfo(result);
        throw TransportError("socket() failed");
    }

    set_nonblocking(sock, true);
    const int rc = ::connect(sock, result->ai_addr, static_cast<int>(result->ai_addrlen));
    ::freeaddrinfo(result);

    bool connected = (rc == 0);
    if (!connected) {
        if (!wait_writable(sock, timeout_ms)) {
            close_socket(sock);
            throw TransportError("connect to " + host + " timed out");
        }
        int so_error = 0;
        socklen_t len = sizeof(so_error);
        ::getsockopt(sock, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so_error), &len);
        if (so_error != 0) {
            close_socket(sock);
            throw TransportError("connect failed (err=" + std::to_string(so_error) + ")");
        }
        connected = true;
    }

    set_nonblocking(sock, false);
    set_nodelay(sock);
    return std::unique_ptr<Connection>(new TcpConnection(sock));
}

std::unique_ptr<Listener> tcp_listen(int port) {
    return tcp_listen(port, std::string());
}

std::unique_ptr<Listener> tcp_listen(int port, const std::string& bind_address) {
    ensure_winsock();

    socket_t sock = create_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == kInvalid) {
        throw TransportError("socket() failed");
    }

    int reuse = 1;
    ::setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
                 sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    if (bind_address.empty()) {
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    } else if (::inet_pton(AF_INET, bind_address.c_str(), &addr.sin_addr) != 1) {
        close_socket(sock);
        throw TransportError("invalid bind address: " + bind_address);
    }
    addr.sin_port = htons(static_cast<unsigned short>(port));
    if (::bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close_socket(sock);
        throw TransportError("bind failed (err=" + std::to_string(sock_errno()) + ")");
    }
    if (::listen(sock, SOMAXCONN) != 0) {
        close_socket(sock);
        throw TransportError("listen failed (err=" + std::to_string(sock_errno()) + ")");
    }

    sockaddr_in bound{};
    socklen_t bound_len = sizeof(bound);
    if (::getsockname(sock, reinterpret_cast<sockaddr*>(&bound), &bound_len) != 0) {
        close_socket(sock);
        throw TransportError("getsockname failed");
    }
    return std::unique_ptr<Listener>(new TcpListener(sock, ntohs(bound.sin_port)));
}

} // namespace vkr::transport
