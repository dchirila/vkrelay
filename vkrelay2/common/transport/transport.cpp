#include "common/transport/transport.hpp"

#include "common/protocol/wire.hpp"

namespace vkr::transport {

void MessageChannel::send(protocol::MessageType type, const json::Value& body) {
    const std::string frame = protocol::encode_frame(protocol::encode_message(type, body));
    conn_.write_all(frame.data(), frame.size());
}

bool MessageChannel::recv(protocol::MessageType& type, json::Value& body) {
    for (;;) {
        std::size_t consumed = 0;
        std::string payload;
        std::string error;
        const protocol::FrameStatus status =
            protocol::try_decode_frame(buffer_, consumed, payload, error);
        if (status == protocol::FrameStatus::Ok) {
            buffer_.erase(0, consumed);
            if (!protocol::decode_message(payload, type, body, error)) {
                throw TransportError("malformed message: " + error);
            }
            return true;
        }
        if (status == protocol::FrameStatus::Error) {
            throw TransportError("framing error: " + error);
        }
        // NeedMore: pull more bytes.
        char chunk[4096];
        const std::size_t n = conn_.read_some(chunk, sizeof(chunk));
        if (n == 0) {
            if (buffer_.empty()) {
                return false; // clean EOF on a frame boundary
            }
            throw TransportError("connection closed mid-frame");
        }
        buffer_.append(chunk, n);
    }
}

AcceptedSession server_handshake(MessageChannel& channel, protocol::IdAllocator& ids,
                                 const std::string& worker_backend,
                                 const protocol::HostDisplay& host_display,
                                 const display::DisplayLayoutDecodeResult& display_layout) {
    protocol::MessageType type = protocol::MessageType::Unknown;
    json::Value body;
    if (!channel.recv(type, body)) {
        throw TransportError("client closed before handshake");
    }
    if (type != protocol::MessageType::Hello) {
        protocol::ErrorMsg err{"expected_hello", "first message must be hello"};
        channel.send(protocol::MessageType::Error, err.to_body());
        throw TransportError("handshake: expected hello");
    }

    AcceptedSession session;
    session.client = protocol::ClientHello::from_body(body);
    if (session.client.protocol_version != protocol::kProtocolVersion) {
        protocol::ErrorMsg err{"version_mismatch",
                               "server speaks protocol " +
                                   std::to_string(protocol::kProtocolVersion) + ", client sent " +
                                   std::to_string(session.client.protocol_version)};
        channel.send(protocol::MessageType::Error, err.to_body());
        throw TransportError("handshake: protocol version mismatch");
    }

    // Validate at the producer boundary, before ServerHello::to_body can serialize a
    // status accidentally marked Valid. Capture failures and producer bugs fail at their source
    // with a named diagnostic; they never reach the client disguised as legacy absence.
    if (display_layout.status != display::LayoutDecodeStatus::Absent) {
        if (display_layout.status != display::LayoutDecodeStatus::Valid) {
            protocol::ErrorMsg err{"display_layout_unavailable", display_layout.reason};
            channel.send(protocol::MessageType::Error, err.to_body());
            throw TransportError("handshake: display layout unavailable: " + display_layout.reason);
        }
        const display::ValidationResult validation =
            display::validate_display_layout(display_layout.layout);
        if (!validation.ok) {
            protocol::ErrorMsg err{"display_layout_invalid", validation.reason};
            channel.send(protocol::MessageType::Error, err.to_body());
            throw TransportError("handshake: invalid produced display layout: " +
                                 validation.reason);
        }
    }

    session.server.protocol_version = protocol::kProtocolVersion;
    session.server.supervisor_session_id = ids.supervisor_session_id();
    session.server.worker_id = ids.next_worker_id();
    session.server.worker_backend = worker_backend;
    session.server.host_display = host_display; // all-zero = unknown (omitted from the body)
    session.server.display_layout = display_layout;
    channel.send(protocol::MessageType::HelloAck, session.server.to_body());
    return session;
}

protocol::ServerHello client_handshake(MessageChannel& channel,
                                       const protocol::ClientHello& hello) {
    channel.send(protocol::MessageType::Hello, hello.to_body());

    protocol::MessageType type = protocol::MessageType::Unknown;
    json::Value body;
    if (!channel.recv(type, body)) {
        throw TransportError("server closed during handshake");
    }
    if (type == protocol::MessageType::Error) {
        const protocol::ErrorMsg err = protocol::ErrorMsg::from_body(body);
        throw TransportError("handshake rejected: " + err.code + " (" + err.message + ")");
    }
    if (type != protocol::MessageType::HelloAck) {
        throw TransportError("handshake: expected hello_ack");
    }
    const protocol::ServerHello server = protocol::ServerHello::from_body(body);
    if (server.protocol_version != protocol::kProtocolVersion) {
        throw TransportError("handshake: server protocol version mismatch");
    }
    return server;
}

} // namespace vkr::transport
